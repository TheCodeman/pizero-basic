/**
 * @file psram_heap.h
 * @brief The Pico's PSRAM calls, for the pico-os sources: the Pi has one big
 * heap, so they are malloc's.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <malloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

static inline void *psram_malloc(size_t n) { return malloc(n); }
static inline void psram_free(void *p) { free(p); }
static inline void *psram_realloc(void *p, size_t n) { return realloc(p, n); }
static inline size_t psram_block_size(const void *p) { return malloc_usable_size((void *)p); }
static inline bool psram_present(void) { return false; }
static inline size_t psram_free_total(void) { return 0; }
