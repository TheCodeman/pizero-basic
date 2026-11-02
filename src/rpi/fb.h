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
