/**
 * @file fb.h
 * @brief The HDMI screen (fb.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdbool.h>

/// Asks the firmware for the 640x480 framebuffer and maps it at dvi_framebuf:
/// NULL, or why not.
const char *fb_init(void);
/// There is a screen.
bool fb_present(void);
