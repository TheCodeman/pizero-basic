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
/// Changes the framebuffer's size (the HDMI mode stays: the GPU scales it):
/// NULL, or why not (then the old size is back).
const char *fb_resize(int w, int h);
/// The monitor's mode, as the firmware set it at power-up.
void fb_display_size(int *w, int *h);
/// There is a screen.
bool fb_present(void);
