/**
 * @file hdmi_sound.c
 * @brief Sound over HDMI on the BCM2835 (see hdmi_sound.h).
 *
 * The firmware has set up the HDMI link (in HDMI mode, not DVI: config.txt's
 * hdmi_drive=2). This sets the HDMI block's audio side to 48 kHz stereo
 * IEC958 (S/PDIF framing) samples, sends the audio infoframe, and feeds the
 * MAI FIFO by DMA from two buffers in turn; the DMA interrupt refills the
 * buffer just played.
 *
 * The registers and the sequence are those of Circle's
 * hdmisoundbasedevice.cpp and soundbasedevice.cpp (R. Stange, GPLv3), and its
 * clock readings come from Linux's clk-bcm2835.c (GPL-2.0+) through Circle.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "hdmi_sound.h"

#include <stdbool.h>
#include <stddef.h>

#include "rpi.h"

// ---- Registers --------------------------------------------------------------------

#define HDMI_BASE (PERI_BASE + 0x902000)
#define HD_BASE (PERI_BASE + 0x808000)
#define CM_BASE (PERI_BASE + 0x101000)
#define DMA_BASE (PERI_BASE + 0x007000)

#define MAI_CHANNEL_MAP (HDMI_BASE + 0x90)
#define MAI_CONFIG (HDMI_BASE + 0x94)
#define AUDIO_PACKET_CONFIG (HDMI_BASE + 0x9C)
#define RAM_PACKET_CONFIG (HDMI_BASE + 0xA0)
#define RAM_PACKET_STATUS (HDMI_BASE + 0xA4)
#define CRP_CONFIG (HDMI_BASE + 0xA8)
#define CTS_0 (HDMI_BASE + 0xAC)
#define CTS_1 (HDMI_BASE + 0xB0)
#define TX_PHY_CONTROL_0 (HDMI_BASE + 0x2C4)
#define RAM_PACKET_AUDIO(n) (HDMI_BASE + 0x490 + 4 * (n))  ///< n 0-8

#define MAI_CONTROL (HD_BASE + 0x14)
#define MAI_THRESHOLD (HD_BASE + 0x18)
#define MAI_FORMAT (HD_BASE + 0x1C)
#define MAI_DATA (HD_BASE + 0x20)
#define MAI_SAMPLE_RATE (HD_BASE + 0x2C)

#define MAI_CTL_RESET (1u << 0)
#define MAI_CTL_ERROR_FULL (1u << 1)
#define MAI_CTL_ERROR_EMPTY (1u << 2)
#define MAI_CTL_ENABLE (1u << 3)
#define MAI_CTL_CHANNELS(n) ((uint32_t)(n) << 4)
#define MAI_CTL_FLUSH (1u << 9)
#define MAI_CTL_WHOLE_SAMPLE (1u << 12)
#define MAI_CTL_CHANNEL_ALIGN (1u << 13)
#define MAI_CTL_DELAYED (1u << 15)

#define RAM_PACKET_ENABLE (1u << 16)
#define RAM_PACKET_AUDIO_ID (1u << 4)  ///< the audio infoframe (packet 4)

#define TX_PHY_RNG_POWER_DOWN (1u << 25)

#define CM_HSMDIV (CM_BASE + 0x08C)
#define A2W_PLLH_CTRLR (CM_BASE + 0x1960)
#define A2W_PLLH_FRACR (CM_BASE + 0x1A60)
#define A2W_PLLH_ANA1 (CM_BASE + 0x1074)

#define DMA_CS(ch) (DMA_BASE + 0x100 * (ch) + 0x00)
#define DMA_CONBLK_AD(ch) (DMA_BASE + 0x100 * (ch) + 0x04)
#define DMA_INT_STATUS (DMA_BASE + 0xFE0)
#define DMA_ENABLE (DMA_BASE + 0xFF0)

#define CS_ACTIVE (1u << 0)
#define CS_END (1u << 1)
#define CS_INT (1u << 2)
#define CS_ERROR (1u << 8)
#define CS_PRIORITY(n) ((uint32_t)(n) << 16)
#define CS_PANIC_PRIORITY(n) ((uint32_t)(n) << 20)
#define CS_WAIT_WRITES (1u << 28)
#define CS_RESET (1u << 31)

#define TI_INTEN (1u << 0)
#define TI_WAIT_RESP (1u << 3)
#define TI_DEST_DREQ (1u << 6)
#define TI_SRC_INC (1u << 8)
#define TI_SRC_WIDTH (1u << 9)
#define TI_PERMAP(n) ((uint32_t)(n) << 16)

#define DREQ_HDMI 17
#define IRQ_DMA0 16  ///< DMA channel n's interrupt is 16 + n (n 0-10 here)

/// The GPU and the DMA engines see the ARM's RAM through the L2 cache here.
#define BUS_L2_ALIAS 0x40000000u
/// Peripherals as the DMA engines see them.
#define BUS_PERI(addr) ((uint32_t)(addr) - PERI_BASE + 0x7E000000u)

// ---- IEC958 framing ---------------------------------------------------------------

#define FRAMES_PER_BLOCK 192   ///< frames in an IEC958 block
#define B_PREAMBLE 0x0F        ///< marks a block's first subframe
/// Each DMA buffer: whole IEC958 blocks (2 x 4 ms).
#define BLOCKS_PER_BUFFER 2
#define BUFFER_FRAMES (FRAMES_PER_BLOCK * BLOCKS_PER_BUFFER)

/// The channel status bits sent in each block's first 40 frames: consumer,
/// PCM, 48 kHz, 24-bit samples.
static const uint8_t channel_status[5] = {0x04, 0, 0, 2, 0x0B | (13 << 4)};

// ---- State ------------------------------------------------------------------------

/// A DMA control block (32-byte aligned).
typedef struct {
    uint32_t ti, src, dst, len, stride, next, reserved[2];
} dma_cb_t;

static dma_cb_t cbs[2] __attribute__((aligned(32)));
static uint32_t buffers[2][BUFFER_FRAMES * 2] __attribute__((aligned(32)));
static int16_t mix[BUFFER_FRAMES][2];
static hdmi_sound_fill_t *fill_fn;
static unsigned dma_ch;
static unsigned next_buffer;  ///< the buffer the DMA finishes next

// ---- Helpers ----------------------------------------------------------------------

static bool wait_bit(uint32_t reg, uint32_t mask, bool set, unsigned ms) {
    while (((REG(reg) & mask) != 0) != set) {
        if (!ms--)
            return false;
        timer_delay_us(1000);
    }
    return true;
}

/// Best n/d approximation of num/den with n <= max_n, d <= max_d (Linux's
/// rational_best_approximation).
static void rational(unsigned long num, unsigned long den, unsigned long max_n,
                     unsigned long max_d, unsigned long *best_n, unsigned long *best_d) {
    unsigned long n = num, d = den, n0 = 0, d1 = 0, n1 = 1, d0 = 1;
    for (;;) {
        if (d == 0)
            break;
        unsigned long dp = d, a = n / d;
        d = n % d;
        n = dp;
        unsigned long n2 = n0 + a * n1, d2 = d0 + a * d1;
        if (n2 > max_n || d2 > max_d) {
            unsigned long t1 = (max_n - n0) / n1, t2 = (max_d - d0) / d1;
            unsigned long t = t1 < t2 ? t1 : t2;
            if (2u * t > a || (2u * t == a && d0 * dp > d1 * d)) {
                n1 = n0 + t * n1;
                d1 = d0 + t * d1;
            }
            break;
        }
        n0 = n1;
        n1 = n2;
        d0 = d1;
        d1 = d2;
    }
    *best_n = n1;
    *best_d = d1;
}

/// The HDMI state machine's clock (from PLLD, 500 MHz on the BCM2835), or 0.
static unsigned long hsm_clock(void) {
    uint32_t div = (REG(CM_HSMDIV) >> 4) & 0xFFF;  // 4.8 fixed point
    return div ? (unsigned long)((500000000ull << 8) / div) : 0;
}

/// The pixel clock (PLLH / 10, from the 19.2 MHz crystal), or 0.
static unsigned long pixel_clock(void) {
    uint32_t ctrl = REG(A2W_PLLH_CTRLR);
    uint32_t fdiv = REG(A2W_PLLH_FRACR) & 0xFFFFF;
    uint32_t ndiv = ctrl & 0x3FF, pdiv = (ctrl >> 12) & 7;
    if (REG(A2W_PLLH_ANA1) & (1u << 11)) {  // the feedback prescaler
        ndiv *= 2;
        fdiv *= 2;
    }
    if (!pdiv)
        return 0;
    uint64_t rate = 19200000ull * (((uint64_t)ndiv << 20) + fdiv) / pdiv;
    return (unsigned long)((rate >> 20) / 10);
}

/// A free DMA channel the firmware lets the ARM use (0-10), or -1.
static int free_dma_channel(void) {
    static volatile uint32_t buf[8] __attribute__((aligned(16)));
    buf[0] = sizeof buf;
    buf[1] = 0;
    buf[2] = 0x00060001;  // get DMA channels
    buf[3] = 4;
    buf[4] = 0;
    buf[5] = 0;
    buf[6] = 0;
    buf[7] = 0;
    uint32_t mask = mbox_property(buf) && (buf[4] & 0x80000000u) ? buf[5] : 0x7F35;
    // Not channel 0 (the firmware's own, despite the mask on some versions).
    for (int ch = 1; ch <= 10; ch++)
        if (mask & (1u << ch))
            return ch;
    return -1;
}

/// The IEC958 subframe for a 16-bit sample in frame `frame` of its block.
static inline uint32_t subframe(int16_t sample, unsigned frame) {
    uint32_t v = ((uint32_t)((int32_t)sample << 8) & 0xFFFFFF) << 4;
    if (frame < 40 && (channel_status[frame / 8] & (1u << (frame % 8))))
        v |= 0x40000000u;
    if (__builtin_parity(v))
        v |= 0x80000000u;
    if (frame == 0)
        v |= B_PREAMBLE;
    return v;
}

/// Makes buffer `b`'s next samples.
static void refill(unsigned b) {
    fill_fn(mix, BUFFER_FRAMES);
    uint32_t *out = buffers[b];
    for (unsigned i = 0; i < BUFFER_FRAMES; i++) {
        unsigned frame = i % FRAMES_PER_BLOCK;
        out[2 * i] = subframe(mix[i][0], frame);
        out[2 * i + 1] = subframe(mix[i][1], frame);
    }
    dcache_clean_invalidate(out, sizeof buffers[b]);
}

/// The DMA interrupt: a buffer has been sent; the other is being sent now.
static void dma_irq(void *unused) {
    (void)unused;
    uint32_t cs = REG(DMA_CS(dma_ch));
    REG(DMA_INT_STATUS) = 1u << dma_ch;
    REG(DMA_CS(dma_ch)) = cs;  // clears INT and END
    refill(next_buffer);
    next_buffer ^= 1;
}

// ---- Starting it ------------------------------------------------------------------

const char *hdmi_sound_start(hdmi_sound_fill_t *fill) {
    if (fill_fn)
        return NULL;
    if (!(REG(RAM_PACKET_CONFIG) & RAM_PACKET_ENABLE))
        return "the display is in DVI mode (no sound)";
    unsigned long audio_clock = hsm_clock(), pix = pixel_clock();
    if (!audio_clock || !pix)
        return "can't read the HDMI clocks";
    int ch = free_dma_channel();
    if (ch < 0)
        return "no DMA channel free";
    dma_ch = (unsigned)ch;

    // The audio side of the HDMI block: 48 kHz stereo PCM.
    unsigned long n, m;
    rational(audio_clock, HDMI_SOUND_RATE, 0xFFFFFF, 0x100, &n, &m);
    uint32_t acr_n = HDMI_SOUND_RATE * 128 / 1000;
    uint32_t cts = (uint32_t)((uint64_t)pix * acr_n / (HDMI_SOUND_RATE * 128));
    REG(MAI_CONTROL) = MAI_CTL_RESET | MAI_CTL_FLUSH | MAI_CTL_DELAYED | MAI_CTL_ERROR_EMPTY |
                       MAI_CTL_ERROR_FULL;
    REG(MAI_SAMPLE_RATE) = (uint32_t)n << 8 | (uint32_t)(m - 1);
    REG(MAI_FORMAT) = 9u << 8 | 2u << 16;  // 48 kHz, PCM
    REG(MAI_THRESHOLD) = 16u << 24 | 16u << 16 | 16u << 8 | 16u;
    REG(MAI_CONFIG) = 1u << 26 | 1u << 27 | 3u;  // bit and format reverse, 2 channels
    REG(MAI_CHANNEL_MAP) = 0x08;                 // left 0, right 1
    REG(AUDIO_PACKET_CONFIG) = 1u << 29 | 1u << 24 | (uint32_t)B_PREAMBLE << 10 | 3u;
    REG(CRP_CONFIG) = 1u << 24 | acr_n;  // external CTS, N
    REG(CTS_0) = cts;
    REG(CTS_1) = cts;

    // The audio infoframe: 2 channels, the rest as the stream says.
    REG(RAM_PACKET_CONFIG) &= ~RAM_PACKET_AUDIO_ID;
    if (wait_bit(RAM_PACKET_STATUS, RAM_PACKET_AUDIO_ID, false, 100)) {
        REG(RAM_PACKET_AUDIO(0)) = 0x0A0184;  // type, version, length
        REG(RAM_PACKET_AUDIO(1)) = 0x0170;    // checksum, channels - 1
        for (int i = 2; i <= 8; i++)
            REG(RAM_PACKET_AUDIO(i)) = 0;
        REG(RAM_PACKET_CONFIG) |= RAM_PACKET_AUDIO_ID;
        wait_bit(RAM_PACKET_STATUS, RAM_PACKET_AUDIO_ID, true, 100);
    }

    // DMA: the two buffers in a ring, an interrupt after each.
    fill_fn = fill;
    refill(0);
    refill(1);
    for (int i = 0; i < 2; i++) {
        cbs[i].ti = TI_PERMAP(DREQ_HDMI) | TI_SRC_WIDTH | TI_SRC_INC | TI_DEST_DREQ | TI_WAIT_RESP |
                    TI_INTEN;
        cbs[i].src = (uint32_t)(uintptr_t)buffers[i] | BUS_L2_ALIAS;
        cbs[i].dst = BUS_PERI(MAI_DATA);
        cbs[i].len = sizeof buffers[i];
        cbs[i].stride = 0;
        cbs[i].next = (uint32_t)(uintptr_t)&cbs[i ^ 1] | BUS_L2_ALIAS;
        cbs[i].reserved[0] = cbs[i].reserved[1] = 0;
    }
    dcache_clean_invalidate(cbs, sizeof cbs);
    next_buffer = 0;
    REG(DMA_ENABLE) |= 1u << dma_ch;
    timer_delay_us(1000);
    REG(DMA_CS(dma_ch)) = CS_RESET;
    while (REG(DMA_CS(dma_ch)) & CS_RESET)
        ;
    irq_connect(IRQ_DMA0 + dma_ch, dma_irq, NULL);
    dmb();
    REG(DMA_CONBLK_AD(dma_ch)) = (uint32_t)(uintptr_t)&cbs[0] | BUS_L2_ALIAS;
    REG(DMA_CS(dma_ch)) = CS_WAIT_WRITES | CS_PANIC_PRIORITY(15) | CS_PRIORITY(1) | CS_ACTIVE;
    dmb();

    // And on.
    REG(TX_PHY_CONTROL_0) &= ~TX_PHY_RNG_POWER_DOWN;
    REG(MAI_CONTROL) = MAI_CTL_CHANNELS(2) | MAI_CTL_WHOLE_SAMPLE | MAI_CTL_CHANNEL_ALIGN |
                       MAI_CTL_ENABLE;
    return NULL;
}
