/**
 * @file pico/stdlib.h
 * @brief The few Pico SDK time calls the shared pico-basic sources use
 * (term_editor.c), on the Pi's system timer, so they build unchanged.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "rpi/rpi.h"

typedef uint64_t absolute_time_t;

static inline absolute_time_t get_absolute_time(void) { return timer_us(); }
static inline absolute_time_t make_timeout_time_ms(uint32_t ms) { return timer_us() + ms * 1000ull; }
static inline bool time_reached(absolute_time_t t) { return timer_us() >= t; }
static inline uint64_t to_us_since_boot(absolute_time_t t) { return t; }
static inline uint32_t time_us_32(void) { return (uint32_t)timer_us(); }
static inline uint64_t time_us_64(void) { return timer_us(); }
static inline void sleep_us(uint64_t us) { timer_delay_us((uint32_t)us); }
static inline void sleep_ms(uint32_t ms) {
    while (ms--)
        timer_delay_us(1000);
}
