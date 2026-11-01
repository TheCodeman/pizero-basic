/**
 * @file uart.c
 * @brief The serial console: the PL011 (UART0) on GPIO14 (TX, pin 8) and
 * GPIO15 (RX, pin 10). On the Zero W the firmware gives the PL011 to
 * Bluetooth; config.txt's dtoverlay=disable-bt and our own pin setup take
 * it back.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rpi.h"

#define UART_DR (UART0_BASE + 0x00)
#define UART_FR (UART0_BASE + 0x18)
#define UART_IBRD (UART0_BASE + 0x24)
#define UART_FBRD (UART0_BASE + 0x28)
#define UART_LCRH (UART0_BASE + 0x2C)
#define UART_CR (UART0_BASE + 0x30)
#define UART_IMSC (UART0_BASE + 0x38)
#define UART_ICR (UART0_BASE + 0x44)

#define FR_RXFE (1u << 4)
#define FR_TXFF (1u << 5)
#define FR_BUSY (1u << 3)

void uart_init(unsigned baud) {
    REG(UART_CR) = 0;  // off while it's set up
    while (REG(UART_FR) & FR_BUSY)
        ;
    gpio_function(14, GPIO_ALT0);
    gpio_function(15, GPIO_ALT0);
    gpio_pull(14, GPIO_PULL_NONE);
    gpio_pull(15, GPIO_PULL_UP);  // an unplugged RX reads idle, not breaks

    // config.txt sets init_uart_clock=48000000; ask in case it didn't.
    uint32_t clk = mbox_clock_rate(2);
    if (clk == 0)
        clk = 48000000;
    // divisor = clk / (16 * baud), with a 6-bit fraction
    uint32_t div64 = (uint32_t)(((uint64_t)clk * 4 + baud / 2) / baud);
    REG(UART_ICR) = 0x7FF;
    REG(UART_IBRD) = div64 >> 6;
    REG(UART_FBRD) = div64 & 63;
    REG(UART_LCRH) = (3u << 5) | (1u << 4);  // 8 bits, FIFOs on
    REG(UART_IMSC) = 0;
    REG(UART_CR) = (1u << 0) | (1u << 8) | (1u << 9);  // UART, TX, RX on
}

void uart_putc(char c) {
    while (REG(UART_FR) & FR_TXFF)
        ;
    REG(UART_DR) = (uint8_t)c;
}

int uart_getc(void) {
    if (REG(UART_FR) & FR_RXFE)
        return -1;
    return (int)(REG(UART_DR) & 0xFF);
}
