/**
 * @file pico/stdio/driver.h
 * @brief The Pico SDK's stdio driver struct, which pico-os's console.h
 * declares one of (unused on the Pi: terminal.c feeds the console).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
typedef struct stdio_driver {
    void (*out_chars)(const char *buf, int len);
    int crlf_enabled;
} stdio_driver_t;
#define PICO_STDIO_ENABLE_CRLF_SUPPORT 0
