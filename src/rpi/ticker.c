/**
 * @file ticker.c
 * @brief A 100 Hz tick from the system timer's compare channel 1 (IRQ 1), and
 * the soft timers that run from it: the screen's cursor blink, USPi's kernel
 * timers, key repeat.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ticker.h"

#include "rpi.h"

#define TIMER_CS (SYSTIMER_BASE + 0x00)
#define TIMER_CLO (SYSTIMER_BASE + 0x04)
#define TIMER_C1 (SYSTIMER_BASE + 0x10)
#define TICK_US (1000000u / TICK_HZ)

static struct {
    ticker_fn fn;
    void *ctx;
    uint32_t due;     ///< the tick it runs at
    uint32_t period;  ///< ticks between runs (0: once)
    bool used;
} timers[TICKER_MAX];

static volatile uint32_t ticks;

uint32_t ticker_ticks(void) { return ticks; }

int ticker_add(uint32_t delay_ticks, uint32_t period_ticks, ticker_fn fn, void *ctx) {
    uint32_t cpsr;
    __asm volatile("mrs %0, cpsr\n cpsid i" : "=r"(cpsr)::"memory");
    int h = -1;
    for (int i = 0; i < TICKER_MAX; i++)
        if (!timers[i].used) {
            timers[i].fn = fn;
            timers[i].ctx = ctx;
            timers[i].due = ticks + (delay_ticks ? delay_ticks : 1);
            timers[i].period = period_ticks;
            timers[i].used = true;
            h = i;
            break;
        }
    if (!(cpsr & 0x80))  // they were on: back on
        __asm volatile("cpsie i" ::: "memory");
    return h;
}

void ticker_cancel(int h) {
    if (h >= 0 && h < TICKER_MAX)
        timers[h].used = false;
}

static void ticker_irq(void *unused) {
    (void)unused;
    REG(TIMER_CS) = 1u << 1;
    uint32_t next = REG(TIMER_C1) + TICK_US;
    // Late (a long time with interrupts off): skip what was missed.
    if ((int32_t)(next - REG(TIMER_CLO)) < 100)
        next = REG(TIMER_CLO) + TICK_US;
    REG(TIMER_C1) = next;
    uint32_t now = ++ticks;
    for (int i = 0; i < TICKER_MAX; i++) {
        if (!timers[i].used || (int32_t)(now - timers[i].due) < 0)
            continue;
        ticker_fn fn = timers[i].fn;
        void *ctx = timers[i].ctx;
        if (!timers[i].period) {
            // Once: the slot is free before the call, which may start
            // another timer in it (USPi's do).
            timers[i].used = false;
            fn(i, ctx);
            continue;
        }
        timers[i].due += timers[i].period;
        if (!fn(i, ctx))
            timers[i].used = false;
    }
}

void ticker_init(void) {
    REG(TIMER_C1) = REG(TIMER_CLO) + TICK_US;
    REG(TIMER_CS) = 1u << 1;
    irq_connect(1, ticker_irq, 0);
}
