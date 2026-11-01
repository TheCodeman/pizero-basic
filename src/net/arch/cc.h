/**
 * @file cc.h
 * @brief lwIP's compiler and platform glue for the Pi (newlib, little-endian ARM).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdio.h>
#include <stdlib.h>

#define LWIP_ERRNO_STDINCLUDE 1
#define LWIP_TIMEVAL_PRIVATE 0
#include <sys/time.h>
#define LWIP_PLATFORM_DIAG(x) do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { printf("lwIP: %s (%s:%d)\n", x, __FILE__, __LINE__); } while (0)
