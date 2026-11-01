/**
 * @file fb.c
 * @brief The HDMI screen: a 640x480 8-bit framebuffer from the firmware,
 * its palette set so each byte is an RGB332 colour (as pico-os's), mapped
 * write-through at dvi_framebuf (0x30000000) so drawing needs no cache
 * flushes.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "fb.h"

#include "rpi.h"

#define FB_W 640
#define FB_H 480
#define FB_VIRT 0x30000000u

static bool fb_ok;

bool fb_present(void) { return fb_ok; }

const char *fb_init(void) {
    static volatile uint32_t m[64] __attribute__((aligned(16)));
    int i = 0;
    m[i++] = 0;  // size, below
    m[i++] = 0;
    m[i++] = 0x48003; m[i++] = 8; m[i++] = 8; m[i++] = FB_W; m[i++] = FB_H;  // physical size
    m[i++] = 0x48004; m[i++] = 8; m[i++] = 8; m[i++] = FB_W; m[i++] = FB_H;  // virtual size
    m[i++] = 0x48009; m[i++] = 8; m[i++] = 8; m[i++] = 0; m[i++] = 0;        // virtual offset
    m[i++] = 0x48005; m[i++] = 4; m[i++] = 4; m[i++] = 8;                     // depth
    int alloc = i;
    m[i++] = 0x40001; m[i++] = 8; m[i++] = 4; m[i++] = 4096; m[i++] = 0;     // allocate
    int pitch = i;
    m[i++] = 0x40008; m[i++] = 4; m[i++] = 0; m[i++] = 0;                     // pitch
    m[i++] = 0;
    m[0] = (uint32_t)i * 4;
    if (!mbox_property(m))
        return "the firmware gave no framebuffer";
    uint32_t base = BUS_TO_ARM(m[alloc + 3]), size = m[alloc + 4];
    if (!base || size < FB_W * FB_H)
        return "the framebuffer is too small";
    if (m[pitch + 3] != FB_W)
        return "the framebuffer's rows aren't 640 bytes";

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

    if (base & 0xFFF)
        return "the framebuffer isn't page aligned";
    if (!mmu_map_write_through(FB_VIRT, base, FB_W * FB_H))
        return "the framebuffer can't be mapped";
    fb_ok = true;
    return 0;
}
