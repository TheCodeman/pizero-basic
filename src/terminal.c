/**
 * @file terminal.c
 * @brief The terminal BASIC talks to: what it prints goes to the serial
 * port and, when there is one, the HDMI screen (pico-os's console, which
 * understands the same ANSI codes); keys come from the serial port and the
 * USB keyboard.
 *
 * Text is code page 437, as on pico-os's screen: bytes 128-255 are drawn as
 * they are on the screen and sent as UTF-8 to the serial terminal.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "terminal.h"

#include <string.h>

#include "rpi/fb.h"
#include "rpi/rpi.h"
#include "screen/console.h"

static char last_out;  ///< the last byte sent, so CR LF isn't doubled
static bool screen_on;

/// Keys from the USB keyboard and the screen's answers, waiting.
#define KEYQ 256
static volatile uint8_t keyq[KEYQ];
static volatile uint32_t keyq_head, keyq_tail;

void terminal_push_key(int c) {
    uint32_t next = (keyq_head + 1) % KEYQ;
    if (next != keyq_tail) {
        keyq[keyq_head] = (uint8_t)c;
        keyq_head = next;
    }
}

/// The screen's answers to ESC [ 6 n (the cursor's place) come in as keys.
static void screen_reply(const char *text, int len) {
    for (int i = 0; i < len; i++)
        terminal_push_key((unsigned char)text[i]);
}

void terminal_screen_start(void) {
    console_init();
    console_set_reply(screen_reply);
    screen_on = true;
}

bool terminal_has_screen(void) { return screen_on; }

/// A code page 437 byte to the serial terminal, as UTF-8.
static void uart_cp437(uint8_t c) {
    char buf[3];
    int n = cp437_utf8(c, buf);
    for (int i = 0; i < n; i++)
        uart_putc(buf[i]);
}

// The serial side is not asked where its cursor is (ESC [ 6 n): the screen
// answers, and two answers would leave one in the input as keys. Only when
// there is no screen does the serial terminal answer.
static const char ask[] = "\x1b[6n";
static int ask_match;  ///< how much of `ask` the serial output has matched

static void uart_out(char c) {
    if (screen_on) {
        if (c == ask[ask_match]) {
            if (++ask_match == 4)
                ask_match = 0;  // drop it
            return;
        }
        for (int i = 0; i < ask_match; i++)  // a partial match that wasn't
            uart_putc(ask[i]);
        ask_match = 0;
        if (c == ask[0]) {
            ask_match = 1;
            return;
        }
    }
    if ((uint8_t)c >= 0x80)
        uart_cp437((uint8_t)c);
    else
        uart_putc(c);
}

static int cp437_from_unicode(unsigned u);
static const uint16_t cp437_low[32];
static const uint16_t cp437_high[129];

// UTF-8 in the output (the full-screen editor sends a control code's
// picture that way) is passed to the serial terminal as it is and drawn as
// its code page 437 character on the screen. Other bytes from 128 up are
// code page 437 themselves.
static uint8_t seq[4];
static int seq_len, seq_want;

/// A byte for both, outside a UTF-8 sequence.
static void out_byte(char c) {
    uart_out(c);
    if (screen_on)
        console_putc(c);
}

/// The sequence wasn't UTF-8 (or isn't a code page 437 character): its
/// bytes are code page 437.
static void seq_flush(void) {
    for (int i = 0; i < seq_len; i++)
        out_byte((char)seq[i]);
    seq_len = seq_want = 0;
}

static void out(char c) {
    uint8_t b = (uint8_t)c;
    if (seq_len) {
        if ((b & 0xC0) == 0x80) {
            seq[seq_len++] = b;
            if (seq_len < seq_want)
                return;
            unsigned u = seq_want == 2 ? (seq[0] & 0x1Fu) << 6 | (seq[1] & 0x3Fu)
                                       : (seq[0] & 0x0Fu) << 12 | (seq[1] & 0x3Fu) << 6 | (seq[2] & 0x3Fu);
            int k = cp437_from_unicode(u);
            if (k < 0) {
                seq_flush();
                return;
            }
            for (int i = 0; i < seq_len; i++)
                uart_putc((char)seq[i]);
            if (screen_on)
                console_put_glyph((uint8_t)k);
            seq_len = seq_want = 0;
            return;
        }
        seq_flush();
    }
    if (b >= 0xC2 && b <= 0xEF) {  // the start of a 2- or 3-byte sequence
        seq[0] = b;
        seq_len = 1;
        seq_want = b >= 0xE0 ? 3 : 2;
        return;
    }
    out_byte(c);
}

void terminal_write(const char *buf, int len) {
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\n' && last_out != '\r')
            out('\r');
        out(buf[i]);
        last_out = buf[i];
    }
}

void terminal_flush(void) {
    if (seq_len)
        seq_flush();
}

void terminal_write_raw(const char *buf, int len) {
    for (int i = 0; i < len; i++)
        uart_putc(buf[i]);
    if (len)
        last_out = buf[len - 1];
}

void terminal_put_glyph(int c) {
    uart_cp437((uint8_t)c);
    if (screen_on)
        console_put_glyph((uint8_t)c);
}

int terminal_getc(void) {
    if (keyq_tail != keyq_head) {
        int c = keyq[keyq_tail];
        keyq_tail = (keyq_tail + 1) % KEYQ;
        return c;
    }
    return uart_getc();
}

int terminal_getc_wait(void) {
    int c;
    while ((c = terminal_getc()) < 0)
        ;
    return c;
}

// ---- code page 437 as Unicode (from pico-basic's basic_port_serial.c) -----------

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

int cp437_utf8(int c, char *out) {
    unsigned u = c >= 0 && c < 32 ? cp437_low[c] : c >= 127 && c < 256 ? cp437_high[c - 127] : (unsigned)c;
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

/// The code page 437 character for a Unicode one (pictures included), or -1.
static int cp437_from_unicode(unsigned u) {
    for (int i = 1; i < 32; i++)
        if (cp437_low[i] == u)
            return i;
    for (int i = 0; i < 129; i++)
        if (cp437_high[i] == u)
            return 127 + i;
    return -1;
}
