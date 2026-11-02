/**
 * @file fb.c
 * @brief The HDMI screen: an 8-bit framebuffer from the firmware, its
 * palette set so each byte is an RGB332 colour (as pico-os's), mapped
 * write-through so drawing needs no cache flushes.
 *
 * The size is the firmware's: the HDMI mode config.txt picks (hdmi_group,
 * hdmi_mode), or framebuffer_width and framebuffer_height for a smaller
 * picture the GPU scales up to the mode. 640x480 at least; anything over
 * DVI_MAX_WIDTH x DVI_MAX_HEIGHT (4K) is halved until it fits.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "fb.h"

#include "dvi.h"
#include "rpi.h"

#define FB_VIRT 0x30000000u

int dvi_width = 640, dvi_height = 480;
uint8_t *dvi_framebuf;
static bool fb_ok;

bool fb_present(void) { return fb_ok; }

/// The size the firmware would give (its "physical display" size): 0x0 if
/// it doesn't say.
static void firmware_size(int *w, int *h) {
    static volatile uint32_t m[8] __attribute__((aligned(16)));
    m[0] = sizeof m;
    m[1] = 0;
    m[2] = 0x40003; m[3] = 8; m[4] = 0; m[5] = 0; m[6] = 0;
    m[7] = 0;
    *w = *h = 0;
    if (mbox_property(m))
        *w = (int)m[5], *h = (int)m[6];
}

/// Asks for a w x h framebuffer: its bus address, size and pitch (0 if none).
static uint32_t allocate(int w, int h, uint32_t *size, uint32_t *pitch_out) {
    static volatile uint32_t m[64] __attribute__((aligned(16)));
    int i = 0;
    m[i++] = 0;  // size, below
    m[i++] = 0;
    m[i++] = 0x48003; m[i++] = 8; m[i++] = 8; m[i++] = w; m[i++] = h;  // physical size
    m[i++] = 0x48004; m[i++] = 8; m[i++] = 8; m[i++] = w; m[i++] = h;  // virtual size
    m[i++] = 0x48009; m[i++] = 8; m[i++] = 8; m[i++] = 0; m[i++] = 0;  // virtual offset
    m[i++] = 0x48005; m[i++] = 4; m[i++] = 4; m[i++] = 8;              // depth
    int alloc = i;
    m[i++] = 0x40001; m[i++] = 8; m[i++] = 4; m[i++] = 4096; m[i++] = 0;  // allocate
    int pitch = i;
    m[i++] = 0x40008; m[i++] = 4; m[i++] = 0; m[i++] = 0;                  // pitch
    m[i++] = 0;
    m[0] = (uint32_t)i * 4;
    if (!mbox_property(m))
        return 0;
    *size = m[alloc + 4];
    *pitch_out = m[pitch + 3];
    return m[alloc + 3];
}

const char *fb_init(void) {
    int w, h;
    firmware_size(&w, &h);
    if (w < 640 || h < 480)
        w = 640, h = 480;
    while (w > DVI_MAX_WIDTH || h > DVI_MAX_HEIGHT)
        w /= 2, h /= 2;
    w &= ~7;  // whole text cells across

    uint32_t base = 0, size = 0, pitch = 0;
    for (int tries = 0; tries < 2; tries++) {
        base = allocate(w, h, &size, &pitch);
        if (!base)
            return "the firmware gave no framebuffer";
        if (pitch == (uint32_t)w)
            break;
        w &= ~31;  // rows padded: ask for a width that needs none
    }
    base = BUS_TO_ARM(base);
    if (pitch != (uint32_t)w)
        return "the framebuffer's rows are padded";
    if (size < (uint32_t)(w * h))
        return "the framebuffer is too small";

    // The palette: byte rrrgggbb is that colour.
    static volatile uint32_t pal[8 + 256] __attribute__((aligned(16)));
    pal[0] = sizeof pal;
    pal[1] = 0;
    pal[2] = 0x4800B;
    pal[3] = 8 + 256 * 4;
    pal[4] = 8 + 256 * 4;
    pal[5] = 0;  // the first entry
    pal[6] = 256;  // how many
    for (int c = 0; c < 256; c++) {
        uint32_t r = ((c >> 5) & 7) * 255 / 7, g = ((c >> 2) & 7) * 255 / 7, b = (c & 3) * 255 / 3;
        pal[7 + c] = 0xFF000000u | b << 16 | g << 8 | r;
    }
    pal[7 + 256] = 0;  // the end tag
    mbox_property(pal);  // a firmware without it keeps its default palette

    dvi_framebuf = mmu_map_write_through(FB_VIRT, base, (uint32_t)(w * h));
    if (!dvi_framebuf)
        return "the framebuffer can't be mapped";
    dvi_width = w;
    dvi_height = h;
    fb_ok = true;
    return 0;
}
