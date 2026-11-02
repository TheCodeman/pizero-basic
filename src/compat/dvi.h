/**
 * @file dvi.h
 * @brief pico-os's framebuffer, as console.c and the BASIC port use it. On
 * the Pi it is the GPU's 8-bit framebuffer (palette set to RGB332), at the
 * size config.txt gives, mapped by fb.c.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// The screen's size, set by fb_init() from the firmware's (640x480 until then).
///@{
extern int dvi_width, dvi_height;
#define DVI_WIDTH dvi_width
#define DVI_HEIGHT dvi_height
///@}
/// The largest screen fb_init() takes; a bigger one is halved until it fits
/// and the GPU scales it up.
///@{
#define DVI_MAX_WIDTH 1920
#define DVI_MAX_HEIGHT 1200
///@}
#define RGB332(r, g, b) ((uint8_t)(((r) << 5) | ((g) << 2) | (b)))

/// The framebuffer, DVI_WIDTH x DVI_HEIGHT bytes (fb.c maps it).
extern uint8_t *dvi_framebuf;

/// pico-os's line and pixel doubling: never on here.
#define dvi_line_double false
#define dvi_pixel_double false
