/**
 * @file audio.h
 * @brief The sound mixer: BEEP's and SOUND's tones, PLAY's note queue and
 * a source such as the MOD player, mixed at 48 kHz for HDMI. A Pi version of
 * pico-os's audio.h (Ken Segler, MIT), without the emulators' sound chips.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// Starts the sound over HDMI. NULL, or why there is none (then the rest
/// still works, silently).
const char *audio_init(void);

/// Plays a sine tone; a new one replaces one still playing. freq 0 or ms 0
/// stops it.
void audio_beep(unsigned freq_hz, unsigned ms);

/// The notes PLAY queues, played one after another in the background.
#define AUDIO_QUEUE_LEN 64
/// Queues a tone (freq_hz 0: a rest); false if the queue is full.
bool audio_play(unsigned freq_hz, unsigned ms);
/// The queued notes not yet finished.
int audio_play_pending(void);
/// Drops the queued notes.
void audio_play_stop(void);

/// A source mixed in (the MOD player): fn gives the next stereo frame at
/// 48 kHz, from the sound interrupt. NULL removes it.
void audio_set_source(bool (*fn)(int32_t *left, int32_t *right));
