/**
 * @file hdmi_sound.h
 * @brief Sound over HDMI on the BCM2835: 48 kHz stereo, sent to the HDMI
 * block's audio FIFO (the MAI) by DMA.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdint.h>

/// The sample rate.
#define HDMI_SOUND_RATE 48000

/// Called from the DMA interrupt for the next `n` stereo frames (left,
/// right): it must fill them all, quickly.
typedef void hdmi_sound_fill_t(int16_t (*frames)[2], int n);

/// Starts the sound. `fill` makes the samples. NULL if it started, or why not
/// (the display is in DVI mode, or doesn't take sound).
const char *hdmi_sound_start(hdmi_sound_fill_t *fill);
