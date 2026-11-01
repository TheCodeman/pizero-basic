/**
 * @file dvi.h
 * @brief pico-os's framebuffer, as console.c and the BASIC port use it. On
 * the Pi it is the GPU's 640x480 8-bit framebuffer (palette set to RGB332),
 * mapped at a fixed address by fb.c.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define DVI_WIDTH 640
#define DVI_HEIGHT 480
#define RGB332(r, g, b) ((uint8_t)(((r) << 5) | ((g) << 2) | (b)))

/// The framebuffer (link.ld puts it at 0x30000000; fb.c maps it).
extern uint8_t dvi_framebuf[DVI_WIDTH * DVI_HEIGHT];

/// pico-os's line and pixel doubling: never on here.
#define dvi_line_double false
#define dvi_pixel_double false
