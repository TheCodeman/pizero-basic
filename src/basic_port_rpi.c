/**
 * @file basic_port_rpi.c
 * @brief basic_port.h for the Raspberry Pi Zero / Zero W, bare metal.
 *
 * Text goes to the serial terminal (ANSI/VT100, GPIO14/15) and the HDMI
 * screen, whose text console, graphics, pages and pictures are pico-os's;
 * keys come from either the serial port or a USB keyboard (USPi). Files are
 * on the SD card. Sound (BEEP, SOUND, PLAY, MUSIC) goes over HDMI. PIN, PIN
 * MODE and the LED work; PWM, I2C, SPI and COM1 come next.
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

#include "audio.h"
#include "board_watchdog.h"
#include "config.h"
#include "dvi.h"
#include "ff.h"
#include "image.h"
#include "mod/mod.h"
#include "mod/xm.h"
#include "rpi/rpi.h"
#include "screen/console.h"
#include "screen/mouse.h"
#include "term_editor.h"
#include "terminal.h"
#include "usb_input.h"

/// What the hardware still to come says.
#define NOT_YET "not available yet on the Pi Zero"

/// Why there is no sound (main.c), or NULL.
extern const char *sound_error;

int port_key(uint32_t timeout_us) {
    usb_input_poll();
    int c = terminal_getc();
    if (c >= 0 || !timeout_us)
        return c;
    uint64_t end = timer_us() + timeout_us;
    while ((c = terminal_getc()) < 0 && timer_us() < end)
        ;
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
static uint8_t *draw_fb = dvi_framebuf;  ///< where drawing goes

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

void port_fill_rect(int x, int y, int w, int h, uint8_t rgb332) {
    uint8_t *row = &draw_fb[y * DVI_WIDTH + x];
    for (int i = 0; i < h; ++i, row += DVI_WIDTH)
        memset(row, rgb332, w);
}

uint8_t port_get_pixel(int x, int y) {
    return draw_fb[y * DVI_WIDTH + x];
}

const char *port_image(const char *path, int x, int y, int w, int h, int sx, int sy, bool fit,
                       int cx0, int cy0, int cx1, int cy1) {
    image_t img;
    const char *err;
    if (!image_load(path, w ? w : 640, h ? h : 480, &img, &err))
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
