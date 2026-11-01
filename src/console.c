/**
 * @file console.c
 * @brief The console's outputs and inputs (see console.h).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "console.h"

#include "rpi/rpi.h"

static char last_out;  ///< the last byte sent, so CR LF isn't doubled

void console_write(const char *buf, int len) {
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\n' && last_out != '\r')
            uart_putc('\r');
        uart_putc(buf[i]);
        last_out = buf[i];
    }
}

void console_write_raw(const char *buf, int len) {
    for (int i = 0; i < len; i++)
        uart_putc(buf[i]);
    if (len)
        last_out = buf[len - 1];
}

int console_getc(void) { return uart_getc(); }

int console_getc_wait(void) {
    int c;
    while ((c = console_getc()) < 0)
        ;
    return c;
}
