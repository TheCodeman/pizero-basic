/**
 * @file irq.c
 * @brief The interrupt dispatcher: start.S's IRQ vector calls irq_handler.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rpi.h"

#define IRQ_PENDING2 (IRQ_BASE + 0x08)

void irq_handler(void) {
    uint32_t p2 = REG(IRQ_PENDING2);
    if (p2 & (1u << (57 - 32)))
        uart_irq();
    dmb();
}

void irq_init(void) {
    __asm volatile("cpsie i" ::: "memory");
}
