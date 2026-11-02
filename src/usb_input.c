/**
 * @file usb_input.c
 * @brief The USB keyboard and mouse through USPi, and the environment USPi
 * needs from us (timers, interrupts, the mailbox, logging).
 *
 * Keys are mapped here from USPi's raw reports (US layout), as pico-os's
 * usb_keyboard.c does: arrows, editing and function keys as the ANSI escape
 * sequences a terminal sends, Ctrl-letter as control codes, Alt-x as Esc x,
 * Caps Lock, key repeat. They go into the same queue as the serial port's.
 * Ctrl-Alt-Del restarts the board.
 *
 * USPi finds devices once, at start-up: plug the keyboard (and a hub for a
 * mouse too) in before power-up.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "usb_input.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "dvi.h"
#include "rpi/rpi.h"
#include "rpi/ticker.h"
#include "screen/console.h"
#include "screen/mouse.h"
#include "terminal.h"
typedef unsigned char u8;  // uspi.h wants it (uspi/types.h clashes with newlib's size_t)
#include "uspi.h"
#include "uspios.h"

// ---- USPi's environment (uspios.h) -----------------------------------------------

void MsDelay(unsigned ms) {
    timer_delay_us(ms * 1000u);
}

void usDelay(unsigned us) {
    timer_delay_us(us);
}

/// USPi's kernel timers are one-shot, in 1/HZ ticks (HZ 100, as ours).
typedef struct {
    TKernelTimerHandler *fn;
    void *param, *context;
} uspi_timer_t;
static uspi_timer_t uspi_timers[TICKER_MAX];

static bool uspi_timer_fire(int h, void *ctx) {
    (void)ctx;
    uspi_timer_t *t = &uspi_timers[h];
    t->fn((unsigned)h + 1, t->param, t->context);
    return false;
}

unsigned StartKernelTimer(unsigned delay, TKernelTimerHandler *fn, void *param, void *context) {
    // Interrupts off: the timer can't fire before its slot is filled in.
    uint32_t cpsr;
    __asm volatile("mrs %0, cpsr\n cpsid i" : "=r"(cpsr)::"memory");
    int h = ticker_add(delay ? delay : 1, 0, uspi_timer_fire, NULL);
    if (h >= 0) {
        uspi_timers[h].fn = fn;
        uspi_timers[h].param = param;
        uspi_timers[h].context = context;
    }
    if (!(cpsr & 0x80))
        __asm volatile("cpsie i" ::: "memory");
    return h >= 0 ? (unsigned)h + 1 : 0;
}

void CancelKernelTimer(unsigned handle) {
    if (handle)
        ticker_cancel((int)handle - 1);
}

void ConnectInterrupt(unsigned irq, TInterruptHandler *fn, void *param) {
    irq_connect(irq, fn, param);
}

int SetPowerStateOn(unsigned device) {
    static volatile uint32_t m[8] __attribute__((aligned(16)));
    m[0] = sizeof m;
    m[1] = 0;
    m[2] = 0x28001;  // set power state
    m[3] = 8;
    m[4] = 8;
    m[5] = device;
    m[6] = 3;  // on, and wait
    m[7] = 0;
    return mbox_property(m) && (m[6] & 1) && !(m[6] & 2);
}

int GetMACAddress(unsigned char buf[6]) {
    static volatile uint32_t m[8] __attribute__((aligned(16)));
    m[0] = sizeof m;
    m[1] = 0;
    m[2] = 0x10003;
    m[3] = 6;
    m[4] = 0;
    m[5] = m[6] = 0;
    m[7] = 0;
    if (!mbox_property(m))
        return 0;
    memcpy(buf, (const void *)&m[5], 6);
    return 1;
}

/// USPi's log: warnings and errors to the serial port (and screen).
void LogWrite(const char *source, unsigned severity, const char *fmt, ...) {
    if (severity > LOG_WARNING)
        return;
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    printf("USB (%s): %s\n", source, line);
}

void uspi_assertion_failed(const char *expr, const char *file, unsigned line) {
    printf("USB: assertion failed: %s at %s:%u\n", expr, file, line);
    for (;;)
        __asm volatile("wfi");
}

void DebugHexdump(const void *buf, unsigned len, const char *source) {
    (void)buf, (void)len, (void)source;
}

// ---- The keyboard ------------------------------------------------------------------

#define CTRL (LCTRL | RCTRL)
#define SHIFT (LSHIFT | RSHIFT)
#define ALTS (ALT | ALTGR)

#define REPEAT_DELAY_TICKS 50  ///< 500 ms before a held key repeats
#define REPEAT_TICKS 3         ///< then every 30 ms

static bool have_keyboard, have_mouse;
static bool caps_lock;
static volatile bool leds_pending;
static uint8_t prev_keys[6];
static volatile uint8_t held_keys[6], held_mods;
static volatile uint8_t repeat_key, repeat_mods;
static uint32_t repeat_at;

/// US layout by HID usage, {unshifted, shifted}: 4-56.
static const char us_keys[57][2] = {
    [4] = {'a', 'A'},   [5] = {'b', 'B'},   [6] = {'c', 'C'},   [7] = {'d', 'D'},
    [8] = {'e', 'E'},   [9] = {'f', 'F'},   [10] = {'g', 'G'},  [11] = {'h', 'H'},
    [12] = {'i', 'I'},  [13] = {'j', 'J'},  [14] = {'k', 'K'},  [15] = {'l', 'L'},
    [16] = {'m', 'M'},  [17] = {'n', 'N'},  [18] = {'o', 'O'},  [19] = {'p', 'P'},
    [20] = {'q', 'Q'},  [21] = {'r', 'R'},  [22] = {'s', 'S'},  [23] = {'t', 'T'},
    [24] = {'u', 'U'},  [25] = {'v', 'V'},  [26] = {'w', 'W'},  [27] = {'x', 'X'},
    [28] = {'y', 'Y'},  [29] = {'z', 'Z'},  [30] = {'1', '!'},  [31] = {'2', '@'},
    [32] = {'3', '#'},  [33] = {'4', '$'},  [34] = {'5', '%'},  [35] = {'6', '^'},
    [36] = {'7', '&'},  [37] = {'8', '*'},  [38] = {'9', '('},  [39] = {'0', ')'},
    [40] = {'\r', '\r'}, [41] = {27, 27},   [42] = {8, 8},      [43] = {'\t', '\t'},
    [44] = {' ', ' '},  [45] = {'-', '_'},  [46] = {'=', '+'},  [47] = {'[', '{'},
    [48] = {']', '}'},  [49] = {'\\', '|'}, [50] = {'#', '~'},  [51] = {';', ':'},
    [52] = {'\'', '"'}, [53] = {'`', '~'},  [54] = {',', '<'},  [55] = {'.', '>'},
    [56] = {'/', '?'},
};

/// The keypad (HID 84-99), Num Lock taken as on: / * - + Enter 1-9 0 .
static const char keypad[16] = {'/', '*', '-', '+', '\r', '1', '2', '3',
                                '4', '5', '6', '7', '8', '9', '0', '.'};

static void put_string(const char *s) {
    while (*s)
        terminal_push_key((unsigned char)*s++);
}

/// Queues what a key types: a character or an escape sequence.
static void put_key(uint8_t key, uint8_t mods) {
    switch (key) {
    case 0x52: put_string("\x1b[A"); return;   // up
    case 0x51: put_string("\x1b[B"); return;   // down
    case 0x4F: put_string("\x1b[C"); return;   // right
    case 0x50: put_string("\x1b[D"); return;   // left
    case 0x4A: put_string("\x1b[H"); return;   // Home
    case 0x4D: put_string("\x1b[F"); return;   // End
    case 0x49: put_string("\x1b[2~"); return;  // Insert
    case 0x4C: put_string("\x1b[3~"); return;  // Delete
    case 0x4B: put_string("\x1b[5~"); return;  // Page Up
    case 0x4E: put_string("\x1b[6~"); return;  // Page Down
    case 0x3A: put_string("\x1bOP"); return;   // F1-F12, as xterm sends them
    case 0x3B: put_string("\x1bOQ"); return;
    case 0x3C: put_string("\x1bOR"); return;
    case 0x3D: put_string("\x1bOS"); return;
    case 0x3E: put_string("\x1b[15~"); return;
    case 0x3F: put_string("\x1b[17~"); return;
    case 0x40: put_string("\x1b[18~"); return;
    case 0x41: put_string("\x1b[19~"); return;
    case 0x42: put_string("\x1b[20~"); return;
    case 0x43: put_string("\x1b[21~"); return;
    case 0x44: put_string("\x1b[23~"); return;
    case 0x45: put_string("\x1b[24~"); return;
    }
    char c = 0;
    if (key < sizeof us_keys / sizeof *us_keys) {
        bool shifted = (mods & SHIFT) != 0;
        if (key >= 4 && key <= 29 && caps_lock)
            shifted = !shifted;
        c = us_keys[key][shifted];
    } else if (key >= 0x54 && key <= 0x63) {
        c = keypad[key - 0x54];
    }
    if (!c)
        return;
    if ((mods & CTRL) && ((c >= 'a' && c <= 'z') || (c >= '@' && c <= '_')))
        c &= 0x1f;  // Ctrl-A is 1, Ctrl-C 3, and so on
    if (mods & ALTS)
        terminal_push_key(27);  // Alt-x as Esc x, as xterm sends it
    terminal_push_key((unsigned char)c);
}

static bool has_key(const uint8_t keys[6], uint8_t key) {
    for (int i = 0; i < 6; i++)
        if (keys[i] == key)
            return true;
    return false;
}

/// USPi's raw report, from its interrupt: a key is newly pressed if it
/// wasn't in the report before.
static void key_report(unsigned char mods, const unsigned char keys[6]) {
    if (keys[0] == 0x01)
        return;  // rollover: too many keys held
    for (int i = 0; i < 6; i++) {
        uint8_t key = keys[i];
        if (!key || has_key(prev_keys, key))
            continue;
        if (key == 0x4C && (mods & CTRL) && (mods & ALTS))
            board_reboot();  // Ctrl-Alt-Del
        if (key == 0x39) {  // Caps Lock
            caps_lock = !caps_lock;
            leds_pending = true;
            continue;
        }
        put_key(key, mods);
        repeat_key = key;
        repeat_mods = mods;
        repeat_at = ticker_ticks() + REPEAT_DELAY_TICKS;
    }
    if (repeat_key && !has_key(keys, repeat_key))
        repeat_key = 0;
    repeat_mods = mods;
    memcpy(prev_keys, keys, 6);
    for (int i = 0; i < 6; i++)
        held_keys[i] = keys[i];
    held_mods = mods;
}

/// Key repeat, from the tick.
static bool repeat_tick(int h, void *ctx) {
    (void)h, (void)ctx;
    if (repeat_key && (int32_t)(ticker_ticks() - repeat_at) >= 0) {
        put_key(repeat_key, repeat_mods);
        repeat_at = ticker_ticks() + REPEAT_TICKS;
    }
    return true;
}

void usb_input_poll(void) {
    if (leds_pending && have_keyboard) {
        leds_pending = false;
        USPiKeyboardSetLEDs(caps_lock ? LED_CAPS_LOCK : 0);
    }
}

/// PC (set 1) scan codes to USB HID usages, for KEYDOWN (pico-os's table).
/// Shift, Ctrl and Alt (42/54, 29, 56) are modifier bits instead.
static const uint8_t scan_to_hid[84] = {
    0, 41, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 45, 46, 42, 43,     // 0-15
    20, 26, 8, 21, 23, 28, 24, 12, 18, 19, 47, 48, 40, 0, 4, 22,      // 16-31
    7, 9, 10, 11, 13, 14, 15, 51, 52, 53, 0, 49, 29, 27, 6, 25,       // 32-47
    5, 17, 16, 54, 55, 56, 0, 85, 0, 44, 57, 58, 59, 60, 61, 62,      // 48-63
    63, 64, 65, 66, 67, 83, 71, 74, 82, 75, 86, 80, 93, 79, 87, 77,   // 64-79
    81, 78, 73, 76,                                                   // 80-83
};

bool usb_key_down(int scan) {
    if (scan == 0)
        return have_keyboard;
    uint8_t mods = held_mods;
    if (scan == 29)
        return mods & CTRL;
    if (scan == 42 || scan == 54)
        return mods & (scan == 42 ? LSHIFT : RSHIFT);
    if (scan == 56)
        return mods & ALTS;
    if (scan < 0 || scan >= (int)sizeof scan_to_hid || !scan_to_hid[scan])
        return false;
    for (int i = 0; i < 6; i++)
        if (held_keys[i] == scan_to_hid[scan])
            return true;
    return false;
}

// ---- The mouse: what pico-os's console.c and BASIC ask (mouse.h) ---------------------

static volatile int mouse_x = 320, mouse_y = 240, mouse_buttons;  // fb_init() may move it
static int pointer_mode = MOUSE_POINTER_OFF;

static void uspi_mouse_report(unsigned buttons, int dx, int dy) {
    int x = mouse_x + dx, y = mouse_y + dy;
    mouse_x = x < 0 ? 0 : x >= DVI_WIDTH ? DVI_WIDTH - 1 : x;
    mouse_y = y < 0 ? 0 : y >= DVI_HEIGHT ? DVI_HEIGHT - 1 : y;
    // USPi: bit 0 left, 1 right, 2 middle, as BASIC's
    mouse_buttons = (int)(buttons & 7);
}

int mouse_count(void) { return have_mouse ? 1 : 0; }

void mouse_get(int *x, int *y, int *buttons) {
    *x = mouse_x;
    *y = mouse_y;
    *buttons = mouse_buttons;
}

int mouse_wheel(void) { return 0; }  // USPi doesn't report the wheel

void mouse_pointer(int mode) {
    pointer_mode = mode;
    if (mode != MOUSE_POINTER_TEXT)
        console_pointer_off();
}

int mouse_pointer_mode(void) { return pointer_mode; }

// ---- Start-up ------------------------------------------------------------------------

const char *usb_input_init(void) {
    if (!USPiInitialize())
        return "the USB controller didn't start";
    have_keyboard = USPiKeyboardAvailable() != 0;
    if (have_keyboard) {
        USPiKeyboardRegisterKeyStatusHandlerRaw(key_report);
        ticker_add(1, 1, repeat_tick, NULL);
    }
    have_mouse = USPiMouseAvailable() != 0;
    if (have_mouse)
        USPiMouseRegisterStatusHandler(uspi_mouse_report);
    return NULL;
}

bool usb_keyboard_present(void) { return have_keyboard; }
bool usb_mouse_present(void) { return have_mouse; }
