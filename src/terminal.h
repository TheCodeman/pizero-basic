/**
 * @file terminal.h
 * @brief The terminal: output to the serial port and the HDMI screen, keys
 * from the serial port and the USB keyboard (terminal.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// Writes `len` bytes ('\n' becomes CR LF; 128-255 are code page 437).
void terminal_write(const char *buf, int len);
/// Writes `len` bytes to the serial port as they are (XMODEM).
void terminal_write_raw(const char *buf, int len);
/// Shows character `c`'s picture (0-255, control codes too) at the cursor.
void terminal_put_glyph(int c);
/// The next key, or -1 if none is waiting.
int terminal_getc(void);
/// Waits for the next key.
int terminal_getc_wait(void);
/// A key from the USB keyboard (from its interrupt).
void terminal_push_key(int c);
/// Starts drawing on the screen too (after fb_init).
void terminal_screen_start(void);
bool terminal_has_screen(void);
/// A code page 437 character as UTF-8; returns how many bytes (3 at most).
int cp437_utf8(int c, char *out);
