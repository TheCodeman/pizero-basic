/**
 * @file fb.h
 * @brief The HDMI screen (fb.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdbool.h>

/// Asks the firmware for a framebuffer at its screen size, maps it at
/// dvi_framebuf and sets DVI_WIDTH and DVI_HEIGHT: NULL, or why not.
const char *fb_init(void);
/// There is a screen.
bool fb_present(void);
/// A new w x h framebuffer (w a multiple of 8, 640x480 to DVI_MAX_WIDTH x
/// DVI_MAX_HEIGHT), mapped at dvi_framebuf in place of the old one; the GPU
/// scales it to the HDMI mode, which stays as it is. NULL, or why not (the
/// old size is kept). The screen's contents are lost.
const char *fb_set_size(int w, int h);
/// The screen's size as the firmware gave it at start-up (the HDMI mode,
/// or config.txt's framebuffer size).
void fb_display_size(int *w, int *h);
