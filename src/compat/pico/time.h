/**
 * @file pico/time.h
 * @brief The Pico SDK's repeating timers, as pico-os's console.c uses them,
 * on the Pi's 100 Hz ticker (so periods are rounded to 10 ms).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "rpi/ticker.h"

typedef struct repeating_timer repeating_timer_t;
typedef bool (*repeating_timer_callback_t)(repeating_timer_t *t);
struct repeating_timer {
    int handle;
    repeating_timer_callback_t callback;
    void *user_data;
};

static inline bool repeating_timer_trampoline_(int h, void *ctx) {
    (void)h;
    repeating_timer_t *t = ctx;
    return t->callback(t);
}

static inline bool add_repeating_timer_ms(int32_t ms, repeating_timer_callback_t cb, void *user,
                                          repeating_timer_t *t) {
    uint32_t period = (uint32_t)(ms < 0 ? -ms : ms) * TICK_HZ / 1000;
    if (!period)
        period = 1;
    t->callback = cb;
    t->user_data = user;
    t->handle = ticker_add(period, period, repeating_timer_trampoline_, t);
    return t->handle >= 0;
}

static inline bool cancel_repeating_timer(repeating_timer_t *t) {
    ticker_cancel(t->handle);
    t->handle = -1;
    return true;
}
