/**
 * @file ticker.h
 * @brief The 100 Hz tick and its soft timers (ticker.c). The callbacks run
 * in the timer interrupt.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define TICK_HZ 100  ///< ticks a second
#define TICKER_MAX 32  ///< timers at once

/// A timer's callback: return false to stop it.
typedef bool (*ticker_fn)(int handle, void *ctx);

/// Starts a timer: first after `delay_ticks`, then every `period_ticks` (0:
/// once). Returns its handle, or -1 if there's no room.
int ticker_add(uint32_t delay_ticks, uint32_t period_ticks, ticker_fn fn, void *ctx);
void ticker_cancel(int handle);
/// Ticks since ticker_init.
uint32_t ticker_ticks(void);
/// Starts the tick (IRQ 1).
void ticker_init(void);
