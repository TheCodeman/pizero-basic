/**
 * @file basic_port_rpi.c
 * @brief basic_port.h for the Raspberry Pi Zero / Zero W, bare metal.
 *
 * Text goes to the serial terminal (ANSI/VT100, GPIO14/15) and the HDMI
 * screen, whose text console, graphics, pages and pictures are pico-os's;
 * keys come from either the serial port or a USB keyboard (USPi). Files are
 * on the SD card. Sound (BEEP, SOUND, PLAY, MUSIC) goes over HDMI. PIN, PIN
 * MODE, PWM, I2C, SPI and the LED work, and COM1 borrows the console's
 * UART while it is open.
 *
 * Much of this is pico-basic's basic_port_serial.c (Ken Segler, MIT): the
 * code page 437 table, the memory calls and the XMODEM glue.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "basic_port.h"
#include "basic.h"

#include <malloc.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio.h"
#include "board_watchdog.h"
#include "config.h"
#include "dvi.h"
#include "ff.h"
#include "image.h"
#include "mod/mod.h"
#include "mod/xm.h"
#include "rpi/fb.h"
#include "rpi/rpi.h"
#include "screen/console.h"
#include "screen/mouse.h"
#include "term_editor.h"
#include "terminal.h"
#include "usb_input.h"
#include "net.h"
#include "rpi/wlan.h"

/// What the hardware still to come says.
#define NOT_YET "not available yet on the Pi Zero"

/// Why there is no sound (main.c), or NULL.
extern const char *sound_error;

int port_key(uint32_t timeout_us) {
    usb_input_poll();
    net_poll();
    int c = terminal_getc();
    if (c >= 0 || !timeout_us)
        return c;
    uint64_t end = timer_us() + timeout_us;
    while ((c = terminal_getc()) < 0 && timer_us() < end)
        net_poll();
    return c;
}

int port_glyph_utf8(int c, char *out) {
    // The full-screen editor sends a control code's picture this way: the
    // serial terminal shows it, and terminal.c draws it on the screen too.
    return cp437_utf8(c, out);
}

void port_put_glyph(int c) {
    terminal_put_glyph(c);
}

// ---- Sound: over HDMI (audio.c) ----------------------------------------------------

bool port_play(unsigned freq_hz, unsigned ms) {
    return audio_play(freq_hz, ms);
}

int port_play_pending(void) {
    return audio_play_pending();
}

void port_play_stop(void) {
    audio_play_stop();
}

void port_beep(unsigned freq_hz, unsigned ms) {
    audio_beep(freq_hz, ms);  // BASIC waits for it
}

// ---- The screen: pico-os's text console and 640x480 RGB332 graphics ------------

void port_get_cell(int row, int col, uint8_t *ch, uint8_t *attr) {
    console_get_cell(row, col, ch, attr);
}

void port_set_cell(int row, int col, uint8_t ch, uint8_t attr) {
    console_set_cell(row, col, ch, attr);
}

bool port_shell(const char *command) {
    (void)command;
    return false;  // no shell under BASIC on the Pi
}

void port_edit(const char *path) {
    term_edit(path, true);
}

void port_edit_file(const char *path) {
    term_edit(path, false);
}

// Screen pages: the page on show is the framebuffer; the others are kept in
// RAM and swapped in by copying when SCREEN changes the visual page (as
// pico-os does).

#define MAX_PAGES 8
#define PAGE_BYTES (DVI_WIDTH * DVI_HEIGHT)

static uint8_t *pages[MAX_PAGES];  ///< NULL until used; stale while on show
static int visual_page;
static uint8_t *draw_fb;  ///< where drawing goes (NULL: the screen)

static bool need_page(int n) {
    if (!pages[n]) {
        pages[n] = malloc(PAGE_BYTES);
        if (!pages[n])
            return false;
        memset(pages[n], 0, PAGE_BYTES);
    }
    return true;
}

const char *port_set_pages(int active, int visual) {
    if (active < 0 || active >= MAX_PAGES || visual < 0 || visual >= MAX_PAGES)
        return "no such page";
    if ((active != visual && !need_page(active)) ||
        (visual != visual_page && (!need_page(visual) || !need_page(visual_page))))
        return "not enough memory for the screen pages";
    if (visual != visual_page) {
        memcpy(pages[visual_page], dvi_framebuf, PAGE_BYTES);
        memcpy(dvi_framebuf, pages[visual], PAGE_BYTES);
        visual_page = visual;
    }
    draw_fb = active == visual ? dvi_framebuf : pages[active];
    return NULL;
}

const char *port_copy_page(int from, int to) {
    if (from < 0 || from >= MAX_PAGES || to < 0 || to >= MAX_PAGES)
        return "no such page";
    if (from == to)
        return NULL;
    uint8_t *src = from == visual_page ? dvi_framebuf : (need_page(from) ? pages[from] : NULL);
    uint8_t *dst = to == visual_page ? dvi_framebuf : (need_page(to) ? pages[to] : NULL);
    if (!src || !dst)
        return "not enough memory for the screen pages";
    memcpy(dst, src, PAGE_BYTES);
    return NULL;
}

void port_reset_pages(void) {
    port_set_pages(0, 0);
    for (int i = 0; i < MAX_PAGES; ++i) {
        free(pages[i]);
        pages[i] = NULL;
    }
}

int port_screen_width(void) { return DVI_WIDTH; }
int port_screen_height(void) { return DVI_HEIGHT; }
int port_text_cols(void) { return CONSOLE_COLS; }
int port_text_rows(void) { return CONSOLE_ROWS; }

const char *port_set_text_size(int cols, int rows) {
    if (!fb_present())
        return NULL;  // the serial terminal's size is its own
    if (rows <= 0) {  // the HDMI mode's shape
        int dw, dh;
        fb_display_size(&dw, &dh);
        rows = (int)((long)cols * 8 * dh / dw / 16);
    }
    cols = cols < 80 ? 80 : cols > CONSOLE_MAX_COLS ? CONSOLE_MAX_COLS : cols;
    rows = rows < 30 ? 30 : rows > CONSOLE_MAX_ROWS ? CONSOLE_MAX_ROWS : rows;
    if (cols == CONSOLE_COLS && rows == CONSOLE_ROWS)
        return NULL;
    port_reset_pages();
    console_freeze(true);  // the cursor's blink and the pointer keep off it while it moves
    const char *why = fb_set_size(cols * 8, rows * 16);
    draw_fb = NULL;
    console_resize();
    console_freeze(false);
    mouse_warp(DVI_WIDTH / 2, DVI_HEIGHT / 2);
    return why;
}

void port_fill_rect(int x, int y, int w, int h, uint8_t rgb332) {
    uint8_t *row = &(draw_fb ? draw_fb : dvi_framebuf)[y * DVI_WIDTH + x];
    for (int i = 0; i < h; ++i, row += DVI_WIDTH)
        memset(row, rgb332, w);
}

uint8_t port_get_pixel(int x, int y) {
    return (draw_fb ? draw_fb : dvi_framebuf)[y * DVI_WIDTH + x];
}

const char *port_image(const char *path, int x, int y, int w, int h, int sx, int sy, bool fit,
                       int cx0, int cy0, int cx1, int cy1) {
    image_t img;
    const char *err;
    if (!image_load(path, w ? w : DVI_WIDTH, h ? h : DVI_HEIGHT, &img, &err))
        return err;
    if (!w) {
        w = img.w * sx;
        h = img.h * sy;
    } else if (fit) {
        image_fit(&img, &x, &y, &w, &h);
    }
    image_clip_t clip = {cx0, cy0, cx1, cy1};
    image_draw(&img, draw_fb, x, y, w, h, &clip);
    image_free(&img);
    return NULL;
}

const char *port_save_image(const char *path) {
    const char *err;
    return image_save_bmp(path, dvi_framebuf, &err) ? NULL : err;
}

// ---- The mouse and the keyboard's held keys (usb_input.c) -------------------------

bool port_mouse(int *x, int *y, int *buttons) {
    mouse_get(x, y, buttons);
    return mouse_count() > 0;
}

int port_mouse_wheel(void) {
    return mouse_wheel();
}

void port_mouse_pointer(bool on) {
    mouse_pointer(on ? MOUSE_POINTER_TEXT : MOUSE_POINTER_OFF);
}

bool port_key_down(int scan) {
    return usb_key_down(scan);
}

// ---- MUSIC: a MOD or XM song in the background (src/mod, pico-os's players) ----------
// As pico-os's port, without its PSRAM refresh: the song is in the heap.

static mod_song_t *music;  ///< a MOD, or
static xm_song_t *xmusic;  ///< an XM
static int music_volume = 100;
static bool music_paused;

void *mod_alloc(size_t size) {
    return malloc(size);
}

void mod_free(void *p) {
    free(p);
}

void *xm_alloc(size_t size) {
    return malloc(size);
}

void xm_free(void *p) {
    free(p);
}

void port_music_stop(void) {
    if (!music && !xmusic)
        return;
    if (music)
        mod_stop();
    if (xmusic)
        xm_stop();
    // The sound interrupt runs to its end before this goes on, so once the
    // source is gone nothing is rendering.
    audio_set_source(NULL);
    mod_set_loop(true);
    mod_pause(false);
    xm_set_loop(true);
    xm_pause(false);
    if (xmusic)
        xm_release();
    mod_song_t *s = music;
    xm_song_t *x = xmusic;
    music = NULL;
    xmusic = NULL;
    if (s) {
        mod_clear(s);
        free(s);
    }
    if (x) {
        xm_clear(x);
        free(x);
    }
}

const char *port_music_play(const char *path, bool loop) {
    port_music_stop();
    if (sound_error)
        return sound_error;
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK)
        return "file not found";
    uint32_t size = f_size(&f);
    uint8_t *data = size && size <= (32u << 20) ? malloc(size) : NULL;
    UINT got = 0;
    bool ok = data && f_read(&f, data, size, &got) == FR_OK && got == size;
    f_close(&f);
    if (!ok) {
        free(data);
        return data ? "can't read the file" : "not enough memory";
    }
    const char *err;
    if (xm_is_xm(data, size)) {
        xm_song_t *x = malloc(sizeof *x);
        err = x ? xm_load(x, data, size) : "not enough memory";
        free(data);
        if (err) {
            if (x) {
                xm_clear(x);
                free(x);
            }
            return err;
        }
        xmusic = x;
    } else {
        mod_song_t *s = malloc(sizeof *s);
        err = s ? mod_load(s, data, size) : "not enough memory";
        free(data);
        if (err) {
            if (s) {
                mod_clear(s);
                free(s);
            }
            return err;
        }
        music = s;
    }
    music_paused = false;
    if (xmusic) {
        xm_set_loop(loop);
        xm_set_volume(music_volume);
        xm_pause(false);
        if (!xm_play(xmusic, 0, 0)) {
            port_music_stop();
            return "not enough memory for the player";
        }
        audio_set_source(xm_render_frame);
    } else {
        mod_set_loop(loop);
        mod_set_volume(music_volume);
        mod_pause(false);
        mod_set_tempo(6, 125);
        mod_play(music, 0, 0, false);
        audio_set_source(mod_render);
    }
    return NULL;
}

void port_music_pause(bool paused) {
    music_paused = paused;
    if (music)
        mod_pause(paused);
    if (xmusic)
        xm_pause(paused);
}

void port_music_volume(int percent) {
    music_volume = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    mod_set_volume(music_volume);
    xm_set_volume(music_volume);
}

void port_music_pos(int order, int row) {
    if (music && order >= 0 && order < music->length)
        mod_play(music, order, row, false);
    if (xmusic && order >= 0 && order < xmusic->length)
        xm_play(xmusic, order, row);
}

int port_music_state(int what) {
    if (xmusic) {
        xm_status_t st;
        xm_get_status(&st);
        switch (what) {
        case 0: return st.playing && !music_paused ? -1 : 0;
        case 1: return st.order;
        case 2: return st.row;
        default: return st.pattern;
        }
    }
    if (!music)
        return 0;
    mod_status_t st;
    mod_get_status(&st);
    switch (what) {
    case 0: return st.state != MOD_STOPPED && !music_paused ? -1 : 0;
    case 1: return st.order;
    case 2: return st.row;
    default: return st.pattern;
    }
}

// ---- Time ------------------------------------------------------------------------

void port_sleep_ms(unsigned ms) {
    uint64_t end = timer_us() + ms * 1000ull;
    while (timer_us() < end) {
        board_watchdog_feed();
        net_poll();
    }
}

double port_seconds(void) {
    return timer_us() / 1e6;
}

// ---- Memory: one big heap (all the ARM's RAM above the program) -------------------

size_t heap_room(void);  ///< syscalls.c: what the heap can still grow by

void *port_alloc(size_t n) {
    return malloc(n ? n : 1);
}

void *port_calloc(size_t count, size_t size) {
    return calloc(count, size);
}

void port_release(void *p) {
    free(p);
}

void *port_realloc(void *p, size_t n) {
    if (!n) {
        free(p);
        return NULL;
    }
    return realloc(p, n);
}

size_t port_free_mem(void) {
    struct mallinfo mi = mallinfo();
    return heap_room() + mi.fordblks;
}

/// BASIC runs on the SVC stack (512 KB, link.ld) and checks it has this much
/// left, for interrupts and the deepest leaf calls.
#define STACK_MARGIN 8192

bool port_stack_low(void) {
    extern char __svc_stack_bottom[];
    char here;
    return &here < __svc_stack_bottom + STACK_MARGIN;
}

// ---- GPIO: the 40-pin header's GPIO 0-27 -----------------------------------------
// The Zero's green ACT LED is on GPIO47, active low; BASIC calls it pin 47.

#define PIN_COUNT 28
#define LED_PIN 47

/// What BASIC has made of each pin.
enum { UNUSED, INPUT, OUTPUT, PULLUP, PULLDOWN, PWM, I2C, SPI, COM };
static bool com_open;  ///< COM1 has the console's UART
static uint8_t pin_use[LED_PIN + 1];
static double pwm_freq[2];  ///< what each PWM channel runs at

int port_pin_count(void) {
    return LED_PIN + 1;  // so PIN 47 (the LED) is in range
}

int port_led_pin(void) {
    return LED_PIN;
}

const char *port_pin_reserved(int pin) {
    if (pin == 14 || pin == 15)
        return "the serial console";
    if (pin == LED_PIN)
        return NULL;
    if (pin >= PIN_COUNT)
        return "not on the header";
    return NULL;
}

/// Frees a pin from PWM, I2C or SPI if BASIC's PIN commands take it over.
static void pin_release(int pin);

void port_pin_mode(int pin, int mode) {
    pin_release(pin);
    switch (mode) {
    case PORT_PIN_OUTPUT:
        gpio_function(pin, GPIO_OUT);
        pin_use[pin] = OUTPUT;
        break;
    case PORT_PIN_PULLUP:
        gpio_function(pin, GPIO_IN);
        gpio_pull(pin, GPIO_PULL_UP);
        pin_use[pin] = PULLUP;
        break;
    case PORT_PIN_PULLDOWN:
        gpio_function(pin, GPIO_IN);
        gpio_pull(pin, GPIO_PULL_DOWN);
        pin_use[pin] = PULLDOWN;
        break;
    default:
        gpio_function(pin, GPIO_IN);
        gpio_pull(pin, GPIO_PULL_NONE);
        pin_use[pin] = INPUT;
        break;
    }
}

void port_pin_write(int pin, int level) {
    if (pin_use[pin] != OUTPUT)
        port_pin_mode(pin, PORT_PIN_OUTPUT);
    gpio_write(pin, pin == LED_PIN ? !level : level);  // the LED is on when low
}

int port_pin_read(int pin) {
    if (pin_use[pin] == UNUSED || pin_use[pin] == PWM)
        port_pin_mode(pin, PORT_PIN_INPUT);
    return gpio_read(pin);
}

/// "1.5k" and so on.
static const char *freq_text(double f, char *out, size_t n) {
    if (f >= 1e6)
        snprintf(out, n, "%gM", f / 1e6);
    else if (f >= 1e3)
        snprintf(out, n, "%gk", f / 1e3);
    else
        snprintf(out, n, "%g", f);
    return out;
}

void port_pwm(int pin, double freq, double duty) {
    int ch = pwm_channel(pin);
    if (ch < 0) {
        if (freq > 0)
            printf("PWM is on GPIO12, 13, 18 and 19 only\n");
        return;
    }
    if (freq <= 0) {
        if (pin_use[pin] == PWM) {
            int partner = pin < 18 ? pin + 6 : pin - 6;
            if (pin_use[partner] != PWM)
                pwm_set(pin, 0, 0);
            gpio_function(pin, GPIO_IN);
            pin_use[pin] = INPUT;
        }
        return;
    }
    int partner = pin < 18 ? pin + 6 : pin - 6;  // 12 and 18, 13 and 19
    if (pin_use[pin] != PWM)
        pin_release(pin);
    double got = pwm_set(pin, freq, duty);
    if (pin_use[partner] == PWM && pwm_freq[ch] != got) {
        char text[16];
        printf("Warning: GPIO%d shares a PWM channel with GPIO%d; both now run at %s Hz\n",
               partner, pin, freq_text(got, text, sizeof text));
    }
    pwm_freq[ch] = got;
    pin_use[pin] = PWM;
}

int port_adc(int pin) {
    (void)pin;
    return -1;  // the Pi has no ADC
}

double port_temperature(void) {
    return mbox_temperature() / 1000.0;
}

/// What a header GPIO is doing, for PINS: "free", "out 1", "I2C1 SDA" and so on.
static void pin_what(int pin, char *what, size_t n) {
    static const char *const names[] = {"", "in", "out", "in up", "in down"};
    if (pin == 14 || pin == 15)
        snprintf(what, n, "%s %s", com_open ? "COM1" : "console", pin == 14 ? "TX" : "RX");
    else if (pin_use[pin] == COM)
        snprintf(what, n, "COM1 %s", pin == 16 ? "CTS" : "RTS");
    else if (pin_use[pin] == UNUSED)
        snprintf(what, n, "free");
    else if (pin_use[pin] == PWM) {
        char text[16];
        snprintf(what, n, "PWM %sHz", freq_text(pwm_freq[pwm_channel(pin)], text, sizeof text));
    } else if (pin_use[pin] == I2C)
        snprintf(what, n, "I2C%d %s", pin / 2, pin & 1 ? "SCL" : "SDA");
    else if (pin_use[pin] == SPI)
        snprintf(what, n, "SPI0 %s", pin == 11 ? "SCK" : pin == 10 ? "MOSI" : "MISO");
    else
        snprintf(what, n, "%s %d", names[pin_use[pin]], gpio_read(pin));
}

// The 40-pin header, physical pin 1 to 40: a GPIO number, or one of these.
enum { HDR_3V3 = -1, HDR_5V = -2, HDR_GND = -3 };
static const signed char header[40] = {
    HDR_3V3, HDR_5V, 2, HDR_5V, 3, HDR_GND, 4, 14, HDR_GND, 15,
    17, 18, 27, HDR_GND, 22, 23, HDR_3V3, 24, 10, HDR_GND,
    9, 25, 11, 8, HDR_GND, 7, 0, 1, 5, HDR_GND,
    6, 12, 13, HDR_GND, 19, 16, 26, 20, HDR_GND, 21,
};

/// A header pin's mark in the drawing: o free, * in use, + power, - ground.
static char pin_mark(int phys) {
    int g = header[phys - 1];
    if (g == HDR_GND)
        return '-';
    if (g < 0)
        return '+';
    return g == 14 || g == 15 || pin_use[g] != UNUSED ? '*' : 'o';
}

/// "GPIO17" or "3V3" for a header pin.
static const char *pin_name(int phys, char *out, size_t n) {
    int g = header[phys - 1];
    snprintf(out, n, "%s", g == HDR_3V3 ? "3V3" : g == HDR_5V ? "5V" : g == HDR_GND ? "GND" : "");
    if (g >= 0)
        snprintf(out, n, "GPIO%d", g);
    return out;
}

void port_pins_list(void) {
    // The board from above, header along the top edge, pin 1 at the SD end.
    char row[2][41];
    for (int i = 0; i < 20; i++) {
        row[0][i * 2] = pin_mark(i * 2 + 2), row[0][i * 2 + 1] = ' ';
        row[1][i * 2] = pin_mark(i * 2 + 1), row[1][i * 2 + 1] = ' ';
    }
    row[0][39] = row[1][39] = '\0';
    printf("  .-------------------------------------------------------------.\n");
    printf("  |    2 [%s] 40           |\n", row[0]);
    printf("  |    1 [%s] 39           |\n", row[1]);
    printf("  |SD                                         Pi Zero W     CAM |\n");
    printf("  '-----[mini HDMI]------------------------[USB]--[PWR]---------'\n\n");

    // The header pin by pin, as you'd see it with the SD card at the top.
    for (int phys = 1; phys < 40; phys += 2) {
        char lname[8], rname[8], lwhat[24] = "", rwhat[24] = "";
        if (header[phys - 1] >= 0)
            pin_what(header[phys - 1], lwhat, sizeof lwhat);
        if (header[phys] >= 0)
            pin_what(header[phys], rwhat, sizeof rwhat);
        printf("%14.14s %6s %2d [%c %c] %-2d %-6s %.14s\n", lwhat, pin_name(phys, lname, sizeof lname), phys,
               pin_mark(phys), pin_mark(phys + 1), phys + 1, pin_name(phys + 1, rname, sizeof rname), rwhat);
    }
    printf("o free  * in use  + power  - ground      GPIO47 (LED) %s\n",
           pin_use[LED_PIN] ? (gpio_read(LED_PIN) ? "off" : "on") : "free");
}

void port_pins_reset(void) {
    port_i2c_close();
    port_spi_close();
    port_serial_close();
    for (int pin = 0; pin < PIN_COUNT; pin++) {
        if (pin_use[pin] == UNUSED)
            continue;
        if (pin_use[pin] == PWM)
            pwm_set(pin, 0, 0);
        gpio_function(pin, GPIO_IN);
        gpio_pull(pin, GPIO_PULL_NONE);
        pin_use[pin] = UNUSED;
    }
    if (pin_use[LED_PIN])
        gpio_write(LED_PIN, 1);  // off
    pin_use[LED_PIN] = UNUSED;
}

// ---- I2C and SPI (pins.c) ------------------------------------------------------------
// I2C0 is SDA GPIO0, SCL GPIO1 (the HAT EEPROM pins, 27 and 28 on the
// header); I2C1 is SDA GPIO2, SCL GPIO3 (pins 3 and 5). SPI0 is SCK GPIO11,
// MOSI GPIO10, MISO GPIO9 (pins 23, 19 and 21); chip selects are PIN's.

static int i2c_bus = -1;  ///< 0 or 1 while open
static bool spi_open;

/// A write kept for the read that follows it (nostop): sent with a
/// repeated start between them.
static uint8_t i2c_pending[16];
static int i2c_pending_n, i2c_pending_addr = -1;

/// Why a pin can't be taken, or NULL.
static const char *pin_free(int pin) {
    const char *why = port_pin_reserved(pin);
    if (why)
        return why;
    if (pin_use[pin] != UNUSED)
        return "a pin is in use by PINMODE/PIN/PWM";
    return NULL;
}

/// Sends a write kept back for a read; its result.
static int i2c_flush(void) {
    if (i2c_pending_addr < 0)
        return 0;
    int r = i2c_write_bytes(i2c_pending_addr, i2c_pending, i2c_pending_n);
    i2c_pending_addr = -1;
    return r;
}

const char *port_i2c_open(int sda, int scl, int khz) {
    port_i2c_close();
    if (!((sda == 0 && scl == 1) || (sda == 2 && scl == 3)))
        return "I2C is SDA GPIO2, SCL GPIO3 (or SDA GPIO0, SCL GPIO1)";
    const char *why = pin_free(sda);
    if (!why)
        why = pin_free(scl);
    if (why)
        return why;
    if (khz < 10 || khz > 1000)
        return "the speed must be 10-1000 kHz";
    i2c_bus = sda / 2;
    i2c_init(i2c_bus, (unsigned)khz);
    gpio_function(sda, GPIO_ALT0);
    gpio_function(scl, GPIO_ALT0);
    gpio_pull(sda, GPIO_PULL_UP);  // GPIO2/3 have 1.8k pull-ups on the board too
    gpio_pull(scl, GPIO_PULL_UP);
    pin_use[sda] = pin_use[scl] = I2C;
    return NULL;
}

void port_i2c_close(void) {
    if (i2c_bus < 0)
        return;
    i2c_flush();
    i2c_deinit();
    for (int pin = i2c_bus * 2; pin <= i2c_bus * 2 + 1; pin++) {
        gpio_function(pin, GPIO_IN);
        gpio_pull(pin, GPIO_PULL_NONE);
        pin_use[pin] = UNUSED;
    }
    i2c_bus = -1;
}

int port_i2c_write(int addr, const uint8_t *data, int n, bool nostop) {
    if (i2c_bus < 0)
        return -2;
    i2c_flush();
    if (nostop && n <= (int)sizeof i2c_pending) {
        // Kept until the read: the BSC makes a repeated start only when the
        // read is started while the write is under way.
        memcpy(i2c_pending, data, (size_t)n);
        i2c_pending_n = n;
        i2c_pending_addr = addr;
        return n;
    }
    return i2c_write_bytes(addr, data, n);
}

int port_i2c_read(int addr, uint8_t *data, int n) {
    if (i2c_bus < 0)
        return -2;
    if (i2c_pending_addr >= 0 && i2c_pending_addr != addr)
        i2c_flush();
    int nw = i2c_pending_addr == addr ? i2c_pending_n : 0;
    i2c_pending_addr = -1;
    return i2c_read_bytes(addr, i2c_pending, nw, data, n);
}

const char *port_spi_open(int sck, int mosi, int miso, int khz, int mode) {
    port_spi_close();
    if (sck == 11 && mosi == 12 && miso == 9)
        mosi = 10;  // SPI OPEN's defaults (basic.c puts MOSI after SCK, as on the Pico)
    if (sck != 11 || mosi != 10 || miso != 9)
        return "SPI is SCK GPIO11, MOSI GPIO10, MISO GPIO9";
    const char *why = pin_free(sck);
    if (!why)
        why = pin_free(mosi);
    if (!why)
        why = pin_free(miso);
    if (why)
        return why;
    if (khz < 1 || khz > 50000 || mode < 0 || mode > 3)
        return "the speed must be 1-50000 kHz and the mode 0-3";
    spi_init((unsigned)khz, mode);
    for (int pin = 9; pin <= 11; pin++) {
        gpio_function(pin, GPIO_ALT0);
        pin_use[pin] = SPI;
    }
    spi_open = true;
    return NULL;
}

void port_spi_close(void) {
    if (!spi_open)
        return;
    for (int pin = 9; pin <= 11; pin++) {
        gpio_function(pin, GPIO_IN);
        gpio_pull(pin, GPIO_PULL_NONE);
        pin_use[pin] = UNUSED;
    }
    spi_open = false;
}

bool port_spi_transfer(const uint8_t *out, uint8_t *in, int n) {
    if (!spi_open)
        return false;
    return spi_xfer(out, in, n);
}

static void pin_release(int pin) {
    if (pin_use[pin] == PWM)
        port_pwm(pin, 0, 0);
    else if (pin_use[pin] == I2C)
        port_i2c_close();
    else if (pin_use[pin] == SPI)
        port_spi_close();
    else if (pin_use[pin] == COM)
        port_serial_flow(false);
}

// ---- Serial port (COM1): the console's PL011 and pins, lent while open --------------
// GPIO14 TX and GPIO15 RX (pins 8 and 10), RTS/CTS on GPIO17/16 (pins 11
// and 36). The serial console stops while COM1 is open: the HDMI screen and
// the USB keyboard go on.

static bool com_flow;

const char *port_serial_open(unsigned baud, int data_bits, char parity, int stop_bits, int tx,
                             int rx) {
    if (tx != 14 || rx != 15)
        return "COM1 is TX GPIO14, RX GPIO15 (the serial console's pins)";
    if (!usb_keyboard_present())
        return "COM1 takes the serial console's pins: it needs a USB keyboard";
    if (baud < 300 || baud > 3000000)
        return "the speed must be 300-3000000";
    if (com_open)
        port_serial_close();
    fflush(stdout);
    uart_com_open(baud, data_bits, parity, stop_bits);
    com_open = true;
    return NULL;
}

const char *port_serial_flow(bool on) {
    if (!com_open)
        return "COM1 isn't open";
    if (on && !com_flow) {
        const char *why = pin_free(16);
        if (!why)
            why = pin_free(17);
        if (why)
            return why;
    }
    uart_com_flow(on);
    if (on)
        pin_use[16] = pin_use[17] = COM;
    if (!on && com_flow) {
        for (int pin = 16; pin <= 17; pin++) {
            gpio_function(pin, GPIO_IN);
            gpio_pull(pin, GPIO_PULL_NONE);
            pin_use[pin] = UNUSED;
        }
    }
    com_flow = on;
    return NULL;
}

void port_serial_break(unsigned ms) {
    if (!com_open)
        return;
    uart_com_break(true);
    port_sleep_ms(ms);
    uart_com_break(false);
}

void port_serial_close(void) {
    if (!com_open)
        return;
    port_serial_flow(false);
    uart_com_close();
    com_open = false;
}

void port_serial_write(const char *data, int n) {
    if (com_open)
        uart_com_write(data, n);
}

int port_serial_getc(void) {
    return com_open ? uart_com_getc() : -1;
}

int port_serial_waiting(void) {
    return com_open ? uart_com_waiting() : 0;
}

int port_serial_room(void) {
    return com_open ? uart_com_room() : 0;
}

// ---- XMODEM, YMODEM and ZMODEM on the console ------------------------------------------

#include "ff.h"
#include "xfer.h"

/// A file being sent or received.
typedef struct {
    FIL f;         ///< the file
    bool writing;  ///< being received
} xfile_t;

/// XRECV has created its file (so a broken download is deleted, but not a
/// file of that name that was there before nothing arrived).
static bool xrecv_created;
/// XRECV's name for the file, used in place of a YMODEM or ZMODEM sender's
/// (NULL once the first file has it).
static const char *xrecv_as;
/// XMODEM's ^Z padding is to come off received files (the other protocols
/// send the size).
static bool xrecv_strip;

/// A byte from the console, waiting up to timeout_ms.
/// @param ctx unused
/// @param timeout_ms how long to wait
/// @return the byte, or XFER_TIMEOUT
static int x_getc(void *ctx, unsigned timeout_ms) {
    (void)ctx;
    board_watchdog_feed();
    int c = port_key(timeout_ms * 1000u);  // input is never translated
    return c < 0 ? XFER_TIMEOUT : c;
}

/// Sends bytes to the console.
/// @param ctx unused
/// @param data the bytes
/// @param len their count
static void x_write(void *ctx, const uint8_t *data, int len) {
    (void)ctx;
    terminal_write_raw((const char *)data, len);  // no CR LF
}

/// Opens the file to send.
/// @param ctx unused
/// @param path its path
/// @param size set to its size
/// @param name set to its name (XMODEM doesn't send it)
/// @param name_size room in name
/// @return the file, or NULL
static void *x_open_read(void *ctx, const char *path, uint32_t *size, char *name, int name_size) {
    (void)ctx;
    xfile_t *x = malloc(sizeof *x);
    if (!x)
        return NULL;
    if (f_open(&x->f, path, FA_READ) != FR_OK) {
        free(x);
        return NULL;
    }
    x->writing = false;
    *size = f_size(&x->f);
    snprintf(name, name_size, "%s", path);
    return x;
}

/// Opens the file to receive into, replacing one of that name (as SAVE does).
/// @param ctx unused
/// @param name its name
/// @param size unused (XMODEM doesn't send it)
/// @param resume unused (ZMODEM only)
/// @param offset set to 0
/// @param saved_as set to the name
/// @param saved_as_size room in saved_as
/// @return the file, or NULL
static void *x_open_write(void *ctx, const char *name, uint32_t size, bool resume, uint32_t *offset,
                          char *saved_as, int saved_as_size) {
    (void)ctx, (void)size, (void)resume;
    xfile_t *x = malloc(sizeof *x);
    if (!x)
        return NULL;
    if (xrecv_as) {  // XRECV's own name for the first file
        name = xrecv_as;
        xrecv_as = NULL;
    }
    if (f_open(&x->f, name, FA_READ | FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        free(x);
        return NULL;
    }
    x->writing = true;
    xrecv_created = true;
    *offset = 0;
    snprintf(saved_as, saved_as_size, "%s", name);
    return x;
}

/// Reads from the file being sent.
/// @param ctx unused
/// @param file the file
/// @param buf where
/// @param len how many
/// @return bytes read, or -1
static int x_read(void *ctx, void *file, uint8_t *buf, int len) {
    (void)ctx;
    UINT got;
    return f_read(&((xfile_t *)file)->f, buf, len, &got) == FR_OK ? (int)got : -1;
}

/// Seeks in the file being sent (ZMODEM only).
/// @param ctx unused
/// @param file the file
/// @param offset where
/// @return done
static bool x_seek(void *ctx, void *file, uint32_t offset) {
    (void)ctx;
    return f_lseek(&((xfile_t *)file)->f, offset) == FR_OK;
}

/// Writes received bytes to the file.
/// @param ctx unused
/// @param file the file
/// @param buf the bytes
/// @param len their count
/// @return written
static bool x_write_file(void *ctx, void *file, const uint8_t *buf, int len) {
    (void)ctx;
    UINT done;
    return f_write(&((xfile_t *)file)->f, buf, len, &done) == FR_OK && done == (UINT)len;
}

/// XMODEM pads the last block with ^Z (SUB): takes them off the end of a
/// received file, so it comes back the size it was sent.
/// @param f the file
static void strip_padding(FIL *f) {
    FSIZE_t size = f_size(f);
    uint8_t tail[128];
    UINT got;
    FSIZE_t start = size > sizeof tail ? size - sizeof tail : 0;
    if (f_lseek(f, start) != FR_OK || f_read(f, tail, (UINT)(size - start), &got) != FR_OK)
        return;
    while (got && tail[got - 1] == 0x1a)
        --got;
    if (start + got < size && f_lseek(f, start + got) == FR_OK)
        f_truncate(f);
}

/// Closes the file; a broken download is deleted.
/// @param ctx unused
/// @param file the file
/// @param ok it all arrived
static void x_close(void *ctx, void *file, bool ok) {
    (void)ctx;
    xfile_t *x = file;
    if (x->writing && ok && xrecv_strip)
        strip_padding(&x->f);
    f_close(&x->f);
    // FatFs keeps no name in a FIL: x_open_write's name is the one the
    // caller passed, so a broken download is deleted there (port_xrecv).
    free(x);
}

/// The transfers' callbacks.
static const xfer_io_t xmodem_io = {NULL,   x_getc, x_write,      x_open_read, x_open_write,
                                    x_read, x_seek, x_write_file, x_close,     NULL};

/// port_xsend's protocol numbers as xfer.c's.
static const xfer_proto_t xfer_protos[] = {XFER_XMODEM_1K, XFER_YMODEM, XFER_ZMODEM};

bool port_xsend(const char *path, int proto, char *msg, int size) {
    fflush(stdout);
    xfer_result_t res;
    const char *paths[1] = {path};
    xfer_send(&xmodem_io, xfer_protos[proto >= 0 && proto <= 2 ? proto : 0], paths, 1, &res);
    snprintf(msg, size, "%s", res.message);
    return res.ok;
}

bool port_xrecv(const char *path, int proto, char *msg, int size) {
    fflush(stdout);
    xfer_result_t res;
    xrecv_created = false;
    xrecv_as = proto == 1 || proto == 2 ? path : NULL;
    xrecv_strip = !xrecv_as;
    xfer_receive(&xmodem_io, proto == 1 ? XFER_YMODEM : proto == 2 ? XFER_ZMODEM : XFER_XMODEM, path, &res);
    xrecv_as = NULL;
    if (!res.ok && xrecv_created)
        f_unlink(path);
    snprintf(msg, size, "%s", res.message);
    return res.ok;
}

// ---- NEOPIXEL (no PIO on the Pi: not available) ------------------------------------

const char *port_pixels_open(int pin, int count, bool rgb) {
    (void)pin, (void)count, (void)rgb;
    return "not available on the Pi Zero";
}
void port_pixel_set(int n, int r, int g, int b) { (void)n, (void)r, (void)g, (void)b; }
void port_pixels_show(void) {}
void port_pixels_bright(int level) { (void)level; }
int port_pixels_count(void) { return 0; }
void port_pixels_close(void) {}

int port_neopixel_pin(void) {
    return config.neopixel;
}

void port_config_show(void) {
    config_print();
}

// ---- Wi-Fi (scanning so far) and MQTT (later) ---------------------------------------

/// Esc or Ctrl-C while the chip works.
static bool wifi_stop_key(void) {
    int c = port_key(0);
    return c == 27 || c == 3;
}

/// Starts the chip if it's off, saying so; false (and why) if it won't.
static bool wifi_start(void) {
    if (wlan_up())
        return true;
    printf("Starting Wi-Fi...");
    fflush(stdout);
    const char *err = net_start();
    printf("\r                  \r");
    if (err) {
        printf("Wi-Fi: %s\n", err);
        if (!strncmp(err, "no " WLAN_FIRMWARE_DIR, 3 + strlen(WLAN_FIRMWARE_DIR)))
            printf("The chip's firmware goes in " WLAN_FIRMWARE_DIR " on the card (see README.md).\n");
        return false;
    }
    return true;
}

static void wifi_status(void) {
    if (!wlan_up()) {
        printf("Wi-Fi is off (WIFI ON starts it, WIFI SCAN lists networks)\n");
        return;
    }
    const uint8_t *m = wlan_mac();
    int rev, id = wlan_chip(&rev);
    printf("Wi-Fi on: chip %d rev %d, MAC %02X:%02X:%02X:%02X:%02X:%02X\n", id, rev, m[0], m[1], m[2], m[3],
           m[4], m[5]);
    int link = net_link();
    if (link == NET_UP)
        printf("Connected to %s, signal %d dBm, IP address %s\n", net_ssid(), net_rssi(), net_ip());
    else if (net_ssid()[0])
        printf("%s: %s%s\n", net_ssid(), net_link_text(link), link < 0 ? " (trying again)" : "");
    else
        printf("Not connected (WIFI_SSID and WIFI_PASSWORD in " CONFIG_PATH ", then WIFI ON)\n");
}

/// WIFI ON: joins the network in the settings, waiting (Esc stops waiting).
static void wifi_on(void) {
    if (!wifi_start())
        return;
    if (!config.wifi_ssid[0]) {
        wifi_status();
        return;
    }
    if (net_link() == NET_UP && !strcmp(net_ssid(), config.wifi_ssid)) {
        wifi_status();
        return;
    }
    const char *err = net_connect(config.wifi_ssid, config.wifi_password);
    if (err) {
        printf("Wi-Fi: %s\n", err);
        return;
    }
    printf("Joining %s...", config.wifi_ssid);
    fflush(stdout);
    uint64_t end = timer_us() + 30000000;
    int link;
    while ((link = net_link()) != NET_UP && link >= 0 && timer_us() < end)
        if (wifi_stop_key())
            break;
    printf("\n");
    if (link != NET_UP && link >= 0)
        printf("Still joining; WIFI says how it goes\n");
    wifi_status();
}

#define SCAN_MAX 32
static wlan_network_t found[SCAN_MAX];  ///< the networks the scan heard, strongest kept
static int found_count;

static void on_found(const wlan_network_t *net) {
    if (!net->ssid[0])
        return;  // a hidden network
    int i;
    for (i = 0; i < found_count; i++)
        if (!strcmp(found[i].ssid, net->ssid))
            break;
    if (i == found_count) {
        if (found_count == SCAN_MAX)
            return;
        found_count++;
        found[i] = *net;
    } else if (net->rssi > found[i].rssi) {
        found[i] = *net;
    }
}

static int by_signal(const void *a, const void *b) {
    return ((const wlan_network_t *)b)->rssi - ((const wlan_network_t *)a)->rssi;
}

static void wifi_scan(void) {
    if (!wifi_start())
        return;
    found_count = 0;
    printf("Scanning...");
    fflush(stdout);
    const char *err = wlan_scan(on_found, wifi_stop_key);
    printf("\r           \r");
    if (err) {
        printf("Scan: %s\n", err);
        if (strcmp(err, "stopped"))
            return;
    }
    qsort(found, (size_t)found_count, sizeof found[0], by_signal);
    static const char *const security[] = {"open", "WEP", "WPA", "WPA2"};
    printf("%-32s  Signal  Ch  Security\n", "Network");
    for (int i = 0; i < found_count; i++)
        printf("%-32s  %4d dBm %2u  %s\n", found[i].ssid, found[i].rssi, found[i].channel,
               security[found[i].security & 3]);
    if (!found_count)
        printf("No networks found\n");
}

bool port_wifi(int what) {
    switch (what) {
    case 0:
        wifi_status();
        break;
    case 1:
        wifi_on();
        break;
    case 2:
        net_stop();
        printf("Wi-Fi off\n");
        break;
    case 3:
        wifi_scan();
        break;
    default: {
        printf("NTP: asking %s...", config.ntp_server[0] ? config.ntp_server : "pool.ntp.org");
        fflush(stdout);
        const char *err = net_ntp(NULL, wifi_stop_key);
        printf("\n");
        if (err) {
            printf("NTP: %s\n", err);
        } else {
            time_t t = time(NULL);
            struct tm tm;
            gmtime_r(&t, &tm);
            printf("The clock is set: %04d-%02d-%02d %02d:%02d:%02d UTC\n", tm.tm_year + 1900, tm.tm_mon + 1,
                   tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        }
        break;
    }
    }
    return true;
}
int port_wifi_value(int n) {
    switch (n) {
    case 0: return net_link() == NET_UP ? -1 : 0;
    case 1: return net_rssi();
    case 2: return net_clock_set() ? -1 : 0;
    case 3: return net_link();
    default: return 0;
    }
}
const char *port_wifi_text(int n) {
    return n == 0 ? net_ip() : n == 1 ? net_ssid() : "";
}
// MQTT: src/net/mqtt_port.c

bool port_clock_12h(void) {
    return config.clock12;
}

// ---- Watchdog: the PM block's (16 us ticks, 20 bits: up to 16.7 s) ---------------------

#define PM_RSTC (PERI_BASE + 0x10001C)
#define PM_WDOG (PERI_BASE + 0x100024)
#define PM_PASSWORD 0x5A000000u

static int watchdog_timeout;  ///< ms while armed, 0 off

/// Loads the counter and (re)arms a full reset when it runs out.
static void watchdog_load(void) {
    uint32_t ticks = (uint32_t)watchdog_timeout * 1000u / 16u;
    REG(PM_WDOG) = PM_PASSWORD | (ticks & 0xFFFFF);
    REG(PM_RSTC) = PM_PASSWORD | (REG(PM_RSTC) & ~0x30u) | 0x20u;
}

bool port_watchdog_reset(void) {
    return false;  // the BCM2835 doesn't keep why it reset where we can see it
}

void port_watchdog_set(int ms) {
    if (ms > 0) {
        watchdog_timeout = ms;
        watchdog_load();
    } else {
        watchdog_timeout = 0;
        REG(PM_RSTC) = PM_PASSWORD | 0x102u;  // reset off
    }
}

void port_watchdog_feed(void) {
    if (watchdog_timeout)
        watchdog_load();
}

void board_watchdog_feed(void) {
    port_watchdog_feed();
}

int port_watchdog_ms(void) {
    return watchdog_timeout;
}

int port_watchdog_max(void) {
    return (int)(0xFFFFFull * 16 / 1000);
}
