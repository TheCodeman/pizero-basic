/**
 * @file irq.c
 * @brief The interrupt dispatcher: start.S's IRQ vector calls irq_handler,
 * which runs the handler connected to each pending interrupt (the UART, the
 * 100 Hz tick, USPi's USB controller).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rpi.h"

#define IRQ_PENDING1 (IRQ_BASE + 0x04)
#define IRQ_PENDING2 (IRQ_BASE + 0x08)
#define IRQ_ENABLE1 (IRQ_BASE + 0x10)
#define IRQ_ENABLE2 (IRQ_BASE + 0x14)

static struct {
    void (*fn)(void *);
    void *param;
} handlers[64];

void irq_connect(unsigned irq, void (*fn)(void *), void *param) {
    if (irq >= 64)
        return;
    handlers[irq].fn = fn;
    handlers[irq].param = param;
    dmb();
    REG(irq < 32 ? IRQ_ENABLE1 : IRQ_ENABLE2) = 1u << (irq % 32);
}

void irq_handler(void) {
    uint32_t pending[2] = {REG(IRQ_PENDING1), REG(IRQ_PENDING2)};
    dmb();
    for (int w = 0; w < 2; w++)
        while (pending[w]) {
            int bit = __builtin_ctz(pending[w]);
            pending[w] &= pending[w] - 1;
            unsigned irq = (unsigned)(w * 32 + bit);
            if (handlers[irq].fn)
                handlers[irq].fn(handlers[irq].param);
        }
    dmb();
}

void irq_init(void) {
    __asm volatile("cpsie i" ::: "memory");
}

// newlib's malloc is called from interrupts too (USPi's), so it runs with
// them off. Nested: malloc can call itself through realloc.
static int lock_depth;
static uint32_t lock_cpsr;

void __malloc_lock(void *reent) {
    (void)reent;
    uint32_t cpsr;
    __asm volatile("mrs %0, cpsr\n cpsid i" : "=r"(cpsr)::"memory");
    if (lock_depth++ == 0)
        lock_cpsr = cpsr;
}

void __malloc_unlock(void *reent) {
    (void)reent;
    if (--lock_depth == 0 && !(lock_cpsr & 0x80))
        __asm volatile("cpsie i" ::: "memory");
}
