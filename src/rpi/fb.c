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

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "dvi.h"
#include "ff.h"
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

/// Gives the framebuffer back, before asking for another size.
static void release(void) {
    static volatile uint32_t m[6] __attribute__((aligned(16)));
    m[0] = sizeof m;
    m[1] = 0;
    m[2] = 0x48001; m[3] = 0; m[4] = 0;
    m[5] = 0;
    mbox_property(m);
}

static int display_w = 640, display_h = 480;  ///< the size the firmware gave at start-up

void fb_display_size(int *w, int *h) {
    *w = display_w;
    *h = display_h;
}

/// Allocates, colours and maps a w x h framebuffer (w a multiple of 8).
static const char *set_size(int w, int h) {
    if (fb_ok)
        release();
    fb_ok = false;
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

    uint8_t *fb = mmu_map_write_through(FB_VIRT, base, (uint32_t)(w * h));
    if (!fb)
        return "the framebuffer can't be mapped";
    dvi_framebuf = fb;
    dvi_width = w;
    dvi_height = h;
    fb_ok = true;
    return 0;
}

const char *fb_set_size(int w, int h) {
    if (!fb_ok)
        return "no screen";
    if (w < 640 || h < 480 || w > DVI_MAX_WIDTH || h > DVI_MAX_HEIGHT || (w & 7))
        return "the size is out of range";
    int old_w = dvi_width, old_h = dvi_height;
    const char *why = set_size(w, h);
    if (why)
        set_size(old_w, old_h);  // back as it was, if the firmware will
    return why;
}

/// The size of an HDMI mode (hdmi_group 1 is CEA, 2 is DMT), or false.
static bool mode_size(int group, int mode, int *w, int *h) {
    static const struct { uint8_t group, mode; uint16_t w, h; } modes[] = {
        {1, 1, 640, 480},    {1, 2, 720, 480},    {1, 3, 720, 480},    {1, 4, 1280, 720},
        {1, 5, 1920, 1080},  {1, 16, 1920, 1080}, {1, 17, 720, 576},   {1, 18, 720, 576},
        {1, 19, 1280, 720},  {1, 20, 1920, 1080}, {1, 31, 1920, 1080}, {1, 32, 1920, 1080},
        {1, 33, 1920, 1080}, {1, 34, 1920, 1080},
        {2, 4, 640, 480},    {2, 8, 800, 600},    {2, 9, 800, 600},    {2, 10, 800, 600},
        {2, 16, 1024, 768},  {2, 17, 1024, 768},  {2, 18, 1024, 768},  {2, 23, 1280, 768},
        {2, 27, 1280, 800},  {2, 28, 1280, 800},  {2, 32, 1280, 960},  {2, 35, 1280, 1024},
        {2, 36, 1280, 1024}, {2, 39, 1360, 768},  {2, 42, 1400, 1050}, {2, 47, 1440, 900},
        {2, 51, 1600, 1200}, {2, 58, 1680, 1050}, {2, 68, 1920, 1200}, {2, 69, 1920, 1200},
        {2, 81, 1366, 768},  {2, 82, 1920, 1080}, {2, 85, 1280, 720},
    };
    for (unsigned i = 0; i < sizeof modes / sizeof modes[0]; i++)
        if (modes[i].group == group && modes[i].mode == mode) {
            *w = modes[i].w;
            *h = modes[i].h;
            return true;
        }
    return false;
}

/// The size config.txt on the card asks for (framebuffer_width/height, or
/// hdmi_group/hdmi_mode), in case the firmware doesn't say: false if none.
/// Lines under a [filter] other than [all], [pi0] and [pi0w] don't count.
static bool config_size(int *w, int *h) {
    FIL f;
    if (f_open(&f, "SD:/config.txt", FA_READ) != FR_OK)
        return false;
    int group = 0, mode = 0, fw = 0, fh = 0;
    bool ours = true;
    char line[128];
    while (f_gets(line, sizeof line, &f)) {
        char *s = line + strspn(line, " \t");
        s[strcspn(s, "#\r\n")] = 0;
        if (*s == '[') {
            ours = !strncasecmp(s, "[all]", 5) || !strncasecmp(s, "[pi0]", 5) ||
                   !strncasecmp(s, "[pi0w]", 6);
            continue;
        }
        char *eq = strchr(s, '=');
        if (!ours || !eq)
            continue;
        *eq = 0;
        int v = atoi(eq + 1);
        s[strcspn(s, " \t")] = 0;
        if (!strcasecmp(s, "hdmi_group"))
            group = v;
        else if (!strcasecmp(s, "hdmi_mode"))
            mode = v;
        else if (!strcasecmp(s, "framebuffer_width"))
            fw = v;
        else if (!strcasecmp(s, "framebuffer_height"))
            fh = v;
    }
    f_close(&f);
    if (fw > 0 && fh > 0) {
        *w = fw;
        *h = fh;
        return true;
    }
    return mode_size(group, mode, w, h);
}

static int firmware_w, firmware_h;  ///< what the firmware said at start-up

void fb_firmware_size(int *w, int *h) {
    *w = firmware_w;
    *h = firmware_h;
}

const char *fb_init(void) {
    int w, h, cw, ch;
    firmware_size(&w, &h);
    firmware_w = w;
    firmware_h = h;
    // The firmware should say config.txt's size; if it says something else
    // (or nothing), config.txt on the card decides.
    if (config_size(&cw, &ch) && (cw != w || ch != h))
        w = cw, h = ch;
    if (w < 640 || h < 480)
        w = 640, h = 480;
    display_w = w;
    display_h = h;
    while (w > DVI_MAX_WIDTH || h > DVI_MAX_HEIGHT)
        w /= 2, h /= 2;
    return set_size(w & ~7, h);  // whole text cells across
}
