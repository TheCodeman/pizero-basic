/**
 * @file fault.c
 * @brief What the exception vectors in start.S call: a crash report on the
 * serial console, and the IRQ dispatcher (nothing uses IRQs yet).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rpi.h"

static void put(const char *s) {
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

static void hex(uint32_t v) {
    static const char d[] = "0123456789abcdef";
    for (int i = 28; i >= 0; i -= 4)
        uart_putc(d[(v >> i) & 15]);
}

/// Prints the fault and stops (start.S has saved r0-r12 and the pc).
void fault_report(int kind, const uint32_t *regs, uint32_t pc) {
    static const char *const names[] = {"undefined instruction", "SVC", "prefetch abort",
                                        "data abort", "?", "FIQ"};
    uint32_t dfsr, dfar, ifsr;
    __asm volatile("mrc p15, 0, %0, c5, c0, 0" : "=r"(dfsr));
    __asm volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(dfar));
    __asm volatile("mrc p15, 0, %0, c5, c0, 1" : "=r"(ifsr));
    put("\n*** ");
    put(names[kind]);
    put(" at pc ");
    hex(pc);
    if (kind == 3) {
        put(", address ");
        hex(dfar);
        put(", DFSR ");
        hex(dfsr);
    } else if (kind == 2) {
        put(", IFSR ");
        hex(ifsr);
    }
    put("\n");
    for (int i = 0; i < 13; i++) {
        put(i < 10 ? " r" : " r");
        uart_putc((char)('0' + i / 10 * (i >= 10)));
        uart_putc((char)('0' + i % 10));
        put("=");
        hex(regs[i]);
        if (i % 4 == 3)
            put("\n");
    }
    put("\nStopped. Power-cycle the board.\n");
    for (;;)
        __asm volatile("wfi");
}

