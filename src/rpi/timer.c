/**
 * @file timer.c
 * @brief The BCM2835 system timer: a free-running 64-bit 1 MHz counter.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rpi.h"

#define TIMER_CLO (SYSTIMER_BASE + 0x04)
#define TIMER_CHI (SYSTIMER_BASE + 0x08)

uint64_t timer_us(void) {
    uint32_t hi, lo;
    do {  // read hi twice so a carry between the halves isn't missed
        hi = REG(TIMER_CHI);
        lo = REG(TIMER_CLO);
    } while (hi != REG(TIMER_CHI));
    return ((uint64_t)hi << 32) | lo;
}

void timer_delay_us(uint32_t us) {
    uint32_t start = REG(TIMER_CLO);
    while ((uint32_t)(REG(TIMER_CLO) - start) < us)
        ;
}
