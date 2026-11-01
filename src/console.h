/**
 * @file console.h
 * @brief The console: output goes to the serial port (and, from step 3, the
 * HDMI screen); keys come from the serial port (and the USB keyboard).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdint.h>

/// Writes `len` bytes ('\n' becomes CR LF on the serial port).
void console_write(const char *buf, int len);
/// Writes `len` bytes as they are (XMODEM).
void console_write_raw(const char *buf, int len);
/// The next key, or -1 if none is waiting.
int console_getc(void);
/// Waits for the next key.
int console_getc_wait(void);
