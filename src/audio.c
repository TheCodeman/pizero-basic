/**
 * @file audio.c
 * @brief The sound mixer (see audio.h). The mixing is pico-os's audio.c
 * (Ken Segler, MIT); here it runs in the HDMI sound's DMA interrupt.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "audio.h"

#include <math.h>

#include "rpi/hdmi_sound.h"
#include "rpi/rpi.h"

#define SINE_LEN 256
#define AMPLITUDE 8000  ///< about -12 dBFS

static int16_t sine[SINE_LEN];
static bool running;  ///< the HDMI sound started

/// The tone playing: its step (16.16 per frame; 0 a rest) and frames left.
static volatile uint32_t beep_step, beep_frames_left;
static volatile bool beep_silent;
static volatile bool note_playing;  ///< the tone came from the queue

/// PLAY's notes: written by BASIC, read by the interrupt.
typedef struct {
    uint32_t step, frames;
} note_t;
static note_t notes[AUDIO_QUEUE_LEN];
static volatile unsigned note_head, note_tail;

static bool (*volatile source)(int32_t *left, int32_t *right);

void audio_set_source(bool (*fn)(int32_t *left, int32_t *right)) {
    source = fn;  // one word: the interrupt sees the old one or the new
}

static uint32_t step_for(unsigned freq_hz) {
    return (uint32_t)(((uint64_t)freq_hz * SINE_LEN << 16) / HDMI_SOUND_RATE);
}

/// Runs with interrupts off (so the interrupt never sees half a change).
static uint32_t irq_off(void) {
    uint32_t cpsr;
    __asm volatile("mrs %0, cpsr\n cpsid i" : "=r"(cpsr)::"memory");
    return cpsr;
}

static void irq_restore(uint32_t cpsr) {
    if (!(cpsr & 0x80))
        __asm volatile("cpsie i" ::: "memory");
}

void audio_beep(unsigned freq_hz, unsigned ms) {
    uint32_t s = irq_off();
    beep_silent = false;
    note_playing = false;
    beep_step = step_for(freq_hz);
    beep_frames_left = freq_hz ? ms * (HDMI_SOUND_RATE / 1000) : 0;
    irq_restore(s);
}

bool audio_play(unsigned freq_hz, unsigned ms) {
    if (!running || !ms)
        return true;  // no sound: the notes go nowhere
    unsigned next = (note_head + 1) % AUDIO_QUEUE_LEN;
    if (next == note_tail)
        return false;
    notes[note_head].step = freq_hz ? step_for(freq_hz) : 0;
    notes[note_head].frames = ms * (HDMI_SOUND_RATE / 1000);
    dmb();
    note_head = next;
    return true;
}

int audio_play_pending(void) {
    uint32_t s = irq_off();
    int n = (int)((note_head + AUDIO_QUEUE_LEN - note_tail) % AUDIO_QUEUE_LEN) + (note_playing ? 1 : 0);
    irq_restore(s);
    return n;
}

void audio_play_stop(void) {
    uint32_t s = irq_off();
    note_tail = note_head;
    if (note_playing) {
        beep_frames_left = 0;
        note_playing = false;
    }
    irq_restore(s);
}

/// Above 24000 the level rises a quarter as fast, so loud music flattens
/// gently instead of clipping hard.
static int16_t soft_limit(int32_t x) {
    if (x > 24000)
        x = 24000 + (x - 24000) / 4;
    else if (x < -24000)
        x = -24000 + (x + 24000) / 4;
    return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

/// The DMA interrupt's call for the next frames.
static void fill(int16_t (*frames)[2], int n) {
    static uint32_t phase;
    bool (*src)(int32_t *, int32_t *) = source;
    for (int i = 0; i < n; i++) {
        int32_t s = 0;
        if (!beep_frames_left) {
            note_playing = false;
            if (note_tail != note_head) {
                note_t *nt = &notes[note_tail];
                beep_step = nt->step;
                beep_silent = nt->step == 0;
                beep_frames_left = nt->frames;
                note_playing = true;
                note_tail = (note_tail + 1) % AUDIO_QUEUE_LEN;
            }
        }
        if (beep_frames_left) {
            if (!beep_silent) {
                s = sine[(phase >> 16) % SINE_LEN];
                phase += beep_step;
            }
            beep_frames_left--;
        }
        int32_t l = s, r = s;
        if (src) {
            int32_t sl, sr;
            src(&sl, &sr);
            l += sl / 2;  // two full channels on a side stay under the limiter
            r += sr / 2;
        }
        frames[i][0] = soft_limit(l);
        frames[i][1] = soft_limit(r);
    }
}

const char *audio_init(void) {
    for (int i = 0; i < SINE_LEN; i++)
        sine[i] = (int16_t)(AMPLITUDE * sinf(6.28318531f * i / SINE_LEN));
    const char *err = hdmi_sound_start(fill);
    running = !err;
    return err;
}
