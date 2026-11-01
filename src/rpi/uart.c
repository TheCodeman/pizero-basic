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

#define FR_CTS (1u << 0)
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

/// The PL011 is the console (not COM1).
static bool console = true;

static void raw_putc(uint8_t c) {
    while (REG(UART_FR) & FR_TXFF)
        ;
    REG(UART_DR) = c;
}

void uart_putc(char c) {
    if (console)
        raw_putc((uint8_t)c);
}

// ---- received bytes: a ring filled by the UART's interrupt ------------------------
// The PL011's FIFO holds only 16 bytes; a pasted listing would overflow it
// while BASIC stores a line.

#define RX_SIZE 4096
static volatile uint8_t rx_buf[RX_SIZE];
static volatile uint32_t rx_head, rx_tail;  // written by the IRQ, read here
static bool rx_irq_on;
/// With COM1's flow control: when the ring is full, leave bytes in the FIFO
/// so it fills and RTS tells the sender to wait.
static bool hold_when_full;

/// Drains the FIFO into the ring (from the IRQ, or by polling).
static void rx_drain(void) {
    while (!(REG(UART_FR) & FR_RXFE)) {
        uint32_t next = (rx_head + 1) % RX_SIZE;
        if (next == rx_tail && hold_when_full) {
            REG(UART_IMSC) = 0;  // uart_getc turns it back on
            return;
        }
        uint8_t c = (uint8_t)REG(UART_DR);
        if (next != rx_tail) {  // full: the byte is lost
            rx_buf[rx_head] = c;
            rx_head = next;
        }
    }
}

void uart_irq(void *unused) {
    (void)unused;
    rx_drain();
    REG(UART_ICR) = (1u << 4) | (1u << 6);  // RX and RX timeout
}

void uart_enable_irq(void) {
    REG(UART_IMSC) = (1u << 4) | (1u << 6);
    irq_connect(57, uart_irq, 0);
    rx_irq_on = true;
}

/// The next byte from the ring (and the FIFO), or -1.
static int ring_getc(void) {
    if (rx_tail == rx_head) {
        // Nothing in the ring: look in the FIFO too, in case bytes are
        // sitting below its interrupt level (QEMU has no RX timeout).
        if (rx_irq_on)
            __asm volatile("cpsid i" ::: "memory");
        rx_drain();
        if (rx_irq_on)
            __asm volatile("cpsie i" ::: "memory");
    }
    if (rx_tail == rx_head)
        return -1;
    int c = rx_buf[rx_tail];
    rx_tail = (rx_tail + 1) % RX_SIZE;
    if (hold_when_full && rx_irq_on)
        REG(UART_IMSC) = (1u << 4) | (1u << 6);  // in case a full ring stopped it
    return c;
}

int uart_getc(void) {
    return console ? ring_getc() : -1;
}

// ---- COM1: the same PL011 and pins, lent to BASIC's OPEN "COM1:" --------------------

/// Sets the baud rate and format, and empties the ring.
static void set_format(unsigned baud, int data_bits, char parity, int stop_bits) {
    while (REG(UART_FR) & FR_BUSY)
        ;
    uint32_t cr = REG(UART_CR);
    REG(UART_CR) = 0;
    uint32_t clk = mbox_clock_rate(2);
    if (clk == 0)
        clk = 48000000;
    uint32_t div64 = (uint32_t)(((uint64_t)clk * 4 + baud / 2) / baud);
    REG(UART_IBRD) = div64 >> 6;
    REG(UART_FBRD) = div64 & 63;
    uint32_t lcrh = (uint32_t)(data_bits - 5) << 5 | 1u << 4;  // FIFOs on
    if (parity == 'E' || parity == 'O')
        lcrh |= 1u << 1 | (parity == 'E' ? 1u << 2 : 0);
    if (stop_bits == 2)
        lcrh |= 1u << 3;
    REG(UART_LCRH) = lcrh;
    REG(UART_ICR) = 0x7FF;
    rx_head = rx_tail = 0;
    REG(UART_CR) = cr;
}

void uart_com_open(unsigned baud, int data_bits, char parity, int stop_bits) {
    console = false;
    set_format(baud, data_bits, parity, stop_bits);
}

void uart_com_flow(bool on) {
    if (on) {
        gpio_function(16, GPIO_ALT3);  // CTS0
        gpio_function(17, GPIO_ALT3);  // RTS0
        gpio_pull(16, GPIO_PULL_DOWN);  // nothing connected: never clear to send
        REG(UART_CR) |= 1u << 14 | 1u << 15;
        hold_when_full = true;
    } else {
        REG(UART_CR) &= ~(1u << 14 | 1u << 15);
        hold_when_full = false;
        if (rx_irq_on)
            REG(UART_IMSC) = (1u << 4) | (1u << 6);
    }
}

void uart_com_break(bool on) {
    if (on)
        while (REG(UART_FR) & FR_BUSY)
            ;
    if (on)
        REG(UART_LCRH) |= 1u;
    else
        REG(UART_LCRH) &= ~1u;
}

void uart_com_close(void) {
    uart_com_flow(false);
    set_format(115200, 8, 'N', 1);
    console = true;
}

void uart_com_write(const char *data, int n) {
    for (int i = 0; i < n; i++)
        raw_putc((uint8_t)data[i]);
}

int uart_com_getc(void) {
    return ring_getc();
}

int uart_com_waiting(void) {
    return (int)((rx_head + RX_SIZE - rx_tail) % RX_SIZE);
}

int uart_com_room(void) {
    return RX_SIZE - 1 - uart_com_waiting();
}
