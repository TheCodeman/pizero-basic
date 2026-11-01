/**
 * @file basic_port_rpi.c
 * @brief basic_port.h for the Raspberry Pi Zero / Zero W, bare metal.
 *
 * Step 2: text goes to and from a terminal (ANSI/VT100) on the serial port
 * (GPIO14/15), and files are on the SD card. Built with BASIC_NO_SCREEN, so
 * graphics, pictures, the mouse and MUSIC give "Feature not available"
 * until step 3 brings the HDMI screen. PIN, PIN MODE and the LED work; PWM,
 * sound, ADC, I2C, SPI and COM1 come in step 4.
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

#include "board_watchdog.h"
#include "config.h"
#include "console.h"
#include "image.h"
#include "rpi/rpi.h"
#include "spr.h"
#include "term_editor.h"

/// What the screen-only features say if asked anyway.
#define NO_SCREEN "not available on a serial terminal"
/// What the hardware still to come says.
#define NOT_YET "not available yet on the Pi Zero"

int port_key(uint32_t timeout_us) {
    int c = console_getc();
    if (c >= 0 || !timeout_us)
        return c;
    uint64_t end = timer_us() + timeout_us;
    while ((c = console_getc()) < 0 && timer_us() < end)
        ;
    return c;
}

/// The IBM PC's pictures for control codes 1-31, as Unicode.
static const uint16_t cp437_low[32] = {
    0x0020, 0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022, 0x25D8, 0x25CB, 0x25D9,
    0x2642, 0x2640, 0x266A, 0x266B, 0x263C, 0x25BA, 0x25C4, 0x2195, 0x203C, 0x00B6, 0x00A7,
    0x25AC, 0x21A8, 0x2191, 0x2193, 0x2192, 0x2190, 0x221F, 0x2194, 0x25B2, 0x25BC,
};

/// The IBM PC's characters 127-255, as Unicode.
static const uint16_t cp437_high[129] = {
    0x2302,
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

int port_glyph_utf8(int c, char *out) {
    // The terminal has no PC character set: send the character's picture as
    // UTF-8 (most terminals are), so CHR$(1) is a smiley and CHR$(219) a block.
    unsigned u = c >= 0 && c < 32 ? cp437_low[c] : c >= 127 && c < 256 ? cp437_high[c - 127] : '?';
    if (u < 0x80) {
        out[0] = (char)u;
        return 1;
    }
    if (u < 0x800) {
        out[0] = (char)(0xC0 | u >> 6);
        out[1] = (char)(0x80 | (u & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | u >> 12);
    out[1] = (char)(0x80 | (u >> 6 & 0x3F));
    out[2] = (char)(0x80 | (u & 0x3F));
    return 3;
}

void port_put_glyph(int c) {
    // putchar, not fwrite: stdout is unbuffered, so it goes out in order.
    char buf[3];
    for (int i = 0, n = port_glyph_utf8(c, buf); i < n; ++i)
        putchar((unsigned char)buf[i]);
}

// ---- Sound (step 4: PWM on GPIO18) -----------------------------------------------

bool port_play(unsigned freq_hz, unsigned ms) {
    (void)freq_hz;
    (void)ms;
    return true;
}

int port_play_pending(void) {
    return 0;
}

void port_play_stop(void) {
}

void port_beep(unsigned freq_hz, unsigned ms) {
    (void)freq_hz;
    port_sleep_ms(ms);
}

// ---- What a screen would do -----------------------------------------------------

void port_get_cell(int row, int col, uint8_t *ch, uint8_t *attr) {
    (void)row;
    (void)col;
    *ch = ' ';  // the terminal can't be read back
    *attr = 7;
}

void port_set_cell(int row, int col, uint8_t ch, uint8_t attr) {
    (void)row;
    (void)col;
    (void)ch;
    (void)attr;
}

bool port_shell(const char *command) {
    (void)command;
    return false;
}

void port_edit(const char *path) {
    term_edit(path, true);
}

void port_edit_file(const char *path) {
    term_edit(path, false);
}

const char *port_set_pages(int active, int visual) {
    return active || visual ? NO_SCREEN : NULL;
}

void port_reset_pages(void) {
}

const char *port_copy_page(int from, int to) {
    (void)from;
    (void)to;
    return NO_SCREEN;
}

void port_fill_rect(int x, int y, int w, int h, uint8_t rgb332) {
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)rgb332;
}

uint8_t port_get_pixel(int x, int y) {
    (void)x;
    (void)y;
    return 0;
}

const char *port_image(const char *path, int x, int y, int w, int h, int sx, int sy, bool fit,
                       int cx0, int cy0, int cx1, int cy1) {
    (void)path, (void)x, (void)y, (void)w, (void)h, (void)sx, (void)sy, (void)fit;
    (void)cx0, (void)cy0, (void)cx1, (void)cy1;
    return NO_SCREEN;
}

const char *port_save_image(const char *path) {
    (void)path;
    return NO_SCREEN;
}

bool image_load(const char *path, int want_w, int want_h, image_t *img, const char **err) {
    (void)path, (void)want_w, (void)want_h;
    memset(img, 0, sizeof *img);
    *err = NO_SCREEN;
    return false;
}

void image_free(image_t *img) {
    (void)img;
}

bool spr_load(const char *path, spr_info_t *info, spr_put_fn put, void *ctx, const char **err) {
    (void)path, (void)info, (void)put, (void)ctx;
    *err = NO_SCREEN;
    return false;
}

bool port_mouse(int *x, int *y, int *buttons) {
    *x = *y = *buttons = 0;
    return false;  // no mouse: MOUSEX, MOUSEY and MOUSEB are 0
}

int port_mouse_wheel(void) {
    return 0;
}

void port_mouse_pointer(bool on) {
    (void)on;
}

bool port_key_down(int scan) {
    (void)scan;
    return false;  // a terminal sends keys, not whether they're held
}

const char *port_music_play(const char *path, bool loop) {
    (void)path;
    (void)loop;
    return NO_SCREEN;
}

void port_music_stop(void) {
}

void port_music_pause(bool paused) {
    (void)paused;
}

void port_music_volume(int percent) {
    (void)percent;
}

void port_music_pos(int order, int row) {
    (void)order;
    (void)row;
}

int port_music_state(int what) {
    (void)what;
    return 0;
}

// ---- Time ------------------------------------------------------------------------

void port_sleep_ms(unsigned ms) {
    uint64_t end = timer_us() + ms * 1000ull;
    while (timer_us() < end)
        board_watchdog_feed();
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

/// The pins BASIC has touched (PINS lists them, the reset puts them back).
static uint64_t pins_used;

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

void port_pin_mode(int pin, int mode) {
    pins_used |= 1ull << pin;
    switch (mode) {
    case PORT_PIN_OUTPUT:
        gpio_function(pin, GPIO_OUT);
        break;
    case PORT_PIN_PULLUP:
        gpio_function(pin, GPIO_IN);
        gpio_pull(pin, GPIO_PULL_UP);
        break;
    case PORT_PIN_PULLDOWN:
        gpio_function(pin, GPIO_IN);
        gpio_pull(pin, GPIO_PULL_DOWN);
        break;
    default:
        gpio_function(pin, GPIO_IN);
        gpio_pull(pin, GPIO_PULL_NONE);
        break;
    }
}

void port_pin_write(int pin, int level) {
    pins_used |= 1ull << pin;
    gpio_function(pin, GPIO_OUT);
    gpio_write(pin, pin == LED_PIN ? !level : level);  // the LED is on when low
}

int port_pin_read(int pin) {
    return gpio_read(pin);
}

void port_pwm(int pin, double freq, double duty) {
    (void)freq;
    (void)duty;
    printf("PWM on GPIO%d: %s\n", pin, NOT_YET);
}

int port_adc(int pin) {
    (void)pin;
    return -1;  // the Pi has no ADC
}

double port_temperature(void) {
    return mbox_temperature() / 1000.0;
}

void port_pins_list(void) {
    printf("GPIO 0-27 on the header; 14 and 15 are the serial console; 47 is the LED\n");
    for (int pin = 0; pin < 64; pin++)
        if (pins_used & (1ull << pin))
            printf("  GPIO%d: %d\n", pin, gpio_read(pin));
}

void port_pins_reset(void) {
    for (int pin = 0; pin < PIN_COUNT; pin++)
        if (pins_used & (1ull << pin)) {
            gpio_function(pin, GPIO_IN);
            gpio_pull(pin, GPIO_PULL_NONE);
        }
    if (pins_used & (1ull << LED_PIN))
        gpio_write(LED_PIN, 1);  // off
    pins_used = 0;
    port_i2c_close();
    port_spi_close();
    port_serial_close();
}

// ---- Serial port, I2C and SPI (step 4) -------------------------------------------------

const char *port_serial_open(unsigned baud, int data_bits, char parity, int stop_bits, int tx,
                             int rx) {
    (void)baud, (void)data_bits, (void)parity, (void)stop_bits, (void)tx, (void)rx;
    return NOT_YET;
}
const char *port_serial_flow(bool on) {
    (void)on;
    return NOT_YET;
}
void port_serial_break(unsigned ms) { (void)ms; }
void port_serial_close(void) {}
void port_serial_write(const char *data, int n) { (void)data, (void)n; }
int port_serial_getc(void) { return -1; }
int port_serial_waiting(void) { return 0; }
int port_serial_room(void) { return 0; }

const char *port_i2c_open(int sda, int scl, int khz) {
    (void)sda, (void)scl, (void)khz;
    return NOT_YET;
}
void port_i2c_close(void) {}
int port_i2c_write(int addr, const uint8_t *data, int n, bool nostop) {
    (void)addr, (void)data, (void)n, (void)nostop;
    return -2;
}
int port_i2c_read(int addr, uint8_t *data, int n) {
    (void)addr, (void)data, (void)n;
    return -2;
}
const char *port_spi_open(int sck, int mosi, int miso, int khz, int mode) {
    (void)sck, (void)mosi, (void)miso, (void)khz, (void)mode;
    return NOT_YET;
}
void port_spi_close(void) {}
bool port_spi_transfer(const uint8_t *out, uint8_t *in, int n) {
    (void)out, (void)in, (void)n;
    return false;
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
    console_write_raw((const char *)data, len);  // no CR LF
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

// ---- Wi-Fi and MQTT (later) ---------------------------------------------------------

bool port_wifi(int what) {
    (void)what;
    return false;
}
int port_wifi_value(int n) {
    (void)n;
    return 0;
}
const char *port_wifi_text(int n) {
    (void)n;
    return "";
}
bool port_mqtt_available(void) { return false; }
const char *port_mqtt_connect(const char *host, int port) {
    (void)host, (void)port;
    return NULL;
}
void port_mqtt_disconnect(void) {}
const char *port_mqtt_publish(const char *topic, const char *payload, int len, int qos, bool retain) {
    (void)topic, (void)payload, (void)len, (void)qos, (void)retain;
    return NULL;
}
const char *port_mqtt_subscribe(const char *topic, int qos, bool sub) {
    (void)topic, (void)qos, (void)sub;
    return NULL;
}
void port_mqtt_status(void) {}
int port_mqtt_value(int n) {
    (void)n;
    return 0;
}
const char *port_mqtt_text(int n, int *len) {
    (void)n;
    *len = 0;
    return "";
}

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
