/**
 * @file emmc.c
 * @brief The SD card in the Zero's slot, through the BCM2835's EMMC
 * controller (an Arasan SDHCI) at 0x20300000: SD and SDHC/SDXC cards, 4-bit
 * bus at up to 25 MHz, data moved by the CPU (no DMA).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "emmc.h"

#include <stdio.h>

#include "rpi.h"

#define EMMC_BASE (PERI_BASE + 0x300000)
#define EMMC_ARG2 (EMMC_BASE + 0x00)
#define EMMC_BLKSIZECNT (EMMC_BASE + 0x04)
#define EMMC_ARG1 (EMMC_BASE + 0x08)
#define EMMC_CMDTM (EMMC_BASE + 0x0C)
#define EMMC_RESP0 (EMMC_BASE + 0x10)
#define EMMC_RESP1 (EMMC_BASE + 0x14)
#define EMMC_RESP2 (EMMC_BASE + 0x18)
#define EMMC_RESP3 (EMMC_BASE + 0x1C)
#define EMMC_DATA (EMMC_BASE + 0x20)
#define EMMC_STATUS (EMMC_BASE + 0x24)
#define EMMC_CONTROL0 (EMMC_BASE + 0x28)
#define EMMC_CONTROL1 (EMMC_BASE + 0x2C)
#define EMMC_INTERRUPT (EMMC_BASE + 0x30)
#define EMMC_IRPT_MASK (EMMC_BASE + 0x34)
#define EMMC_IRPT_EN (EMMC_BASE + 0x38)
#define EMMC_CONTROL2 (EMMC_BASE + 0x3C)
#define EMMC_SLOTISR_VER (EMMC_BASE + 0xFC)

// STATUS
#define SR_CMD_INHIBIT (1u << 0)
#define SR_DAT_INHIBIT (1u << 1)

// INTERRUPT
#define INT_CMD_DONE (1u << 0)
#define INT_DATA_DONE (1u << 1)
#define INT_WRITE_RDY (1u << 4)
#define INT_READ_RDY (1u << 5)
#define INT_ERROR (1u << 15)
#define INT_ERROR_MASK 0xFFFF0000u
#define INT_CMD_TIMEOUT (1u << 16)
#define INT_DATA_TIMEOUT (1u << 20)

// CONTROL0
#define C0_HCTL_DWIDTH (1u << 1)  // 4-bit bus

// CONTROL1
#define C1_CLK_INTLEN (1u << 0)
#define C1_CLK_STABLE (1u << 1)
#define C1_CLK_EN (1u << 2)
#define C1_TOUNIT_MAX (0xEu << 16)
#define C1_SRST_HC (1u << 24)
#define C1_SRST_CMD (1u << 25)
#define C1_SRST_DATA (1u << 26)

// CMDTM fields
#define CMD_INDEX(n) ((uint32_t)(n) << 24)
#define CMD_RSPNS_NONE 0u
#define CMD_RSPNS_136 (1u << 16)
#define CMD_RSPNS_48 (2u << 16)
#define CMD_RSPNS_48B (3u << 16)
#define CMD_CRCCHK (1u << 19)
#define CMD_IXCHK (1u << 20)
#define CMD_ISDATA (1u << 21)
#define TM_BLKCNT_EN (1u << 1)
#define TM_AUTO_CMD12 (1u << 2)
#define TM_DAT_DIR_READ (1u << 4)
#define TM_MULTI_BLOCK (1u << 5)

#define R1 (CMD_RSPNS_48 | CMD_CRCCHK | CMD_IXCHK)
#define R1B (CMD_RSPNS_48B | CMD_CRCCHK | CMD_IXCHK)
#define R2 (CMD_RSPNS_136 | CMD_CRCCHK)
#define R3 CMD_RSPNS_48
#define R6 (CMD_RSPNS_48 | CMD_CRCCHK | CMD_IXCHK)
#define R7 (CMD_RSPNS_48 | CMD_CRCCHK | CMD_IXCHK)

#define CMD_GO_IDLE (CMD_INDEX(0) | CMD_RSPNS_NONE)
#define CMD_ALL_SEND_CID (CMD_INDEX(2) | R2)
#define CMD_SEND_REL_ADDR (CMD_INDEX(3) | R6)
#define CMD_CARD_SELECT (CMD_INDEX(7) | R1B)
#define CMD_SEND_IF_COND (CMD_INDEX(8) | R7)
#define CMD_SEND_CSD (CMD_INDEX(9) | R2)
#define CMD_STOP_TRANS (CMD_INDEX(12) | R1B)
#define CMD_SET_BLOCKLEN (CMD_INDEX(16) | R1)
#define CMD_READ_SINGLE (CMD_INDEX(17) | R1 | CMD_ISDATA | TM_DAT_DIR_READ)
#define CMD_READ_MULTI (CMD_INDEX(18) | R1 | CMD_ISDATA | TM_DAT_DIR_READ | TM_BLKCNT_EN | \
                        TM_MULTI_BLOCK | TM_AUTO_CMD12)
#define CMD_WRITE_SINGLE (CMD_INDEX(24) | R1 | CMD_ISDATA)
#define CMD_WRITE_MULTI (CMD_INDEX(25) | R1 | CMD_ISDATA | TM_BLKCNT_EN | TM_MULTI_BLOCK | \
                         TM_AUTO_CMD12)
#define CMD_APP_CMD (CMD_INDEX(55) | R1)
#define ACMD_SET_BUS_WIDTH (CMD_INDEX(6) | R1)
#define ACMD_SEND_OP_COND (CMD_INDEX(41) | R3)

static struct {
    bool ready;      ///< initialised
    bool sdhc;       ///< block addressed (SDHC/SDXC) rather than byte addressed
    uint32_t rca;    ///< the card's relative address (top 16 bits)
    uint32_t base_hz;  ///< the controller's clock
    uint32_t sectors;  ///< the card's size in 512-byte sectors
    bool v3;         ///< host controller spec 3 (10-bit divider)
} card;

/// Waits for `mask` bits in `reg` to clear; false after `us`.
static bool wait_clear(uint32_t reg, uint32_t mask, uint32_t us) {
    uint64_t end = timer_us() + us;
    while (REG(reg) & mask)
        if (timer_us() > end)
            return false;
    return true;
}

/// Waits for an interrupt flag in `mask` (or an error); returns the flags
/// seen, 0 on a timeout. The flags waited for are cleared.
static uint32_t wait_int(uint32_t mask, uint32_t us) {
    uint64_t end = timer_us() + us;
    uint32_t r;
    while (!((r = REG(EMMC_INTERRUPT)) & (mask | INT_ERROR)))
        if (timer_us() > end)
            return 0;
    if (r & INT_ERROR) {
        REG(EMMC_INTERRUPT) = r;
        return r | INT_ERROR;
    }
    REG(EMMC_INTERRUPT) = mask & r;
    return r;
}

/// Resets the command and data lines after an error.
static void reset_lines(void) {
    REG(EMMC_CONTROL1) |= C1_SRST_CMD | C1_SRST_DATA;
    wait_clear(EMMC_CONTROL1, C1_SRST_CMD | C1_SRST_DATA, 100000);
    REG(EMMC_INTERRUPT) = 0xFFFFFFFFu;
}

/// Sends a command; false if it failed. `resp` gets RESP0.
static bool command(uint32_t cmd, uint32_t arg, uint32_t *resp) {
    if (!wait_clear(EMMC_STATUS, SR_CMD_INHIBIT, 100000))
        return false;
    if ((cmd & CMD_RSPNS_48B) == CMD_RSPNS_48B || (cmd & CMD_ISDATA))
        if (!wait_clear(EMMC_STATUS, SR_DAT_INHIBIT, 500000))
            return false;
    REG(EMMC_INTERRUPT) = 0xFFFFFFFFu;
    REG(EMMC_ARG1) = arg;
    REG(EMMC_CMDTM) = cmd;
    uint32_t r = wait_int(INT_CMD_DONE, 500000);
    if (!r || (r & INT_ERROR)) {
        reset_lines();
        return false;
    }
    if (resp)
        *resp = REG(EMMC_RESP0);
    return true;
}

static bool app_command(uint32_t cmd, uint32_t arg, uint32_t *resp) {
    return command(CMD_APP_CMD, card.rca, NULL) && command(cmd, arg, resp);
}

/// Sets the SD clock to at most `hz`.
static bool set_clock(uint32_t hz) {
    if (!wait_clear(EMMC_STATUS, SR_CMD_INHIBIT | SR_DAT_INHIBIT, 100000))
        return false;
    REG(EMMC_CONTROL1) &= ~C1_CLK_EN;
    timer_delay_us(10);
    uint32_t div = (card.base_hz + 2 * hz - 1) / (2 * hz);  // SDCLK = base / (2 * div)
    if (div > 0x3FF)
        div = 0x3FF;
    if (!card.v3) {  // spec 2: a power of 2, 8 bits
        uint32_t p = 1;
        while (p < div && p < 0x80)
            p <<= 1;
        div = p;
    }
    uint32_t c1 = REG(EMMC_CONTROL1) & ~0xFFE0u;
    c1 |= ((div & 0xFF) << 8) | (((div >> 8) & 3) << 6);
    REG(EMMC_CONTROL1) = c1;
    timer_delay_us(10);
    REG(EMMC_CONTROL1) |= C1_CLK_EN;
    timer_delay_us(10);
    return true;
}

/// Bits hi..lo of the CSD: the R2 response holds its bits 127-8, shifted
/// down 8, in RESP0 (lowest) to RESP3.
static uint32_t csd_bits(const uint32_t r[4], int hi, int lo) {
    uint32_t v = 0;
    for (int b = hi; b >= lo; b--) {
        int n = b - 8;
        v = (v << 1) | ((r[n / 32] >> (n % 32)) & 1);
    }
    return v;
}

/// The card's size, from the CSD just read.
static void read_size(void) {
    uint32_t r[4] = {REG(EMMC_RESP0), REG(EMMC_RESP1), REG(EMMC_RESP2), REG(EMMC_RESP3)};
    if (csd_bits(r, 127, 126) == 1) {  // CSD 2.0 (SDHC, SDXC)
        card.sectors = (csd_bits(r, 69, 48) + 1) * 1024;
    } else {  // CSD 1.0
        uint32_t c_size = csd_bits(r, 73, 62), mult = csd_bits(r, 49, 47);
        uint32_t bl_len = csd_bits(r, 83, 80);
        uint64_t bytes = ((uint64_t)(c_size + 1) << (mult + 2)) << bl_len;
        card.sectors = (uint32_t)(bytes / 512);
    }
}

const char *emmc_init(void) {
    card.ready = false;
    card.rca = 0;
    card.base_hz = mbox_clock_rate(1);  // the EMMC clock
    if (card.base_hz == 0)
        card.base_hz = 250000000;
    card.v3 = ((REG(EMMC_SLOTISR_VER) >> 16) & 0xFF) >= 2;

    // The firmware has set GPIO 48-53 for the card already (ALT3).
    REG(EMMC_CONTROL0) = 0;
    REG(EMMC_CONTROL1) = C1_SRST_HC;
    if (!wait_clear(EMMC_CONTROL1, C1_SRST_HC, 100000))
        return "the SD controller didn't reset";
    REG(EMMC_CONTROL2) = 0;
    REG(EMMC_CONTROL1) = C1_CLK_INTLEN | C1_TOUNIT_MAX;
    timer_delay_us(10);
    if (!set_clock(400000))
        return "the SD clock didn't start";
    uint64_t end = timer_us() + 100000;
    while (!(REG(EMMC_CONTROL1) & C1_CLK_STABLE))
        if (timer_us() > end)
            return "the SD clock isn't stable";
    REG(EMMC_IRPT_EN) = 0;  // flags only, no interrupts to the ARM
    REG(EMMC_IRPT_MASK) = 0xFFFFFFFFu;
    REG(EMMC_INTERRUPT) = 0xFFFFFFFFu;
    timer_delay_us(1000);  // 74 clocks before the first command

    command(CMD_GO_IDLE, 0, NULL);
    uint32_t r = 0;
    bool v2 = command(CMD_SEND_IF_COND, 0x1AA, &r) && (r & 0xFFF) == 0x1AA;

    // ACMD41 until the card is ready (up to a second).
    uint32_t ocr = 0;
    end = timer_us() + 1000000;
    for (;;) {
        if (!app_command(ACMD_SEND_OP_COND, 0x00FF8000u | (v2 ? 0x40000000u : 0), &ocr))
            return "none";
        if (ocr & 0x80000000u)
            break;
        if (timer_us() > end)
            return "the card didn't power up";
        timer_delay_us(10000);
    }
    card.sdhc = (ocr & 0x40000000u) != 0;

    if (!command(CMD_ALL_SEND_CID, 0, NULL))
        return "no CID";
    if (!command(CMD_SEND_REL_ADDR, 0, &r))
        return "no address";
    card.rca = r & 0xFFFF0000u;
    if (!command(CMD_SEND_CSD, card.rca, NULL))
        return "no CSD";
    read_size();
    if (!command(CMD_CARD_SELECT, card.rca, NULL))
        return "it can't be selected";
    if (!card.sdhc && !command(CMD_SET_BLOCKLEN, 512, NULL))
        return "it won't take 512-byte blocks";

    // 4-bit bus, then full speed.
    if (app_command(ACMD_SET_BUS_WIDTH, 2, NULL))
        REG(EMMC_CONTROL0) |= C0_HCTL_DWIDTH;
    set_clock(25000000);
    card.ready = true;
    return NULL;
}

bool emmc_ready(void) { return card.ready; }

uint32_t emmc_sectors(void) { return card.sectors; }

/// Moves `count` 512-byte blocks between the card and `buf` (any alignment).
static bool transfer(bool write, uint8_t *buf, uint32_t sector, uint32_t count) {
    if (!card.ready)
        return false;
    while (count) {
        uint32_t n = count > 0xFFFF ? 0xFFFF : count;
        uint32_t addr = card.sdhc ? sector : sector * 512;
        REG(EMMC_BLKSIZECNT) = (n << 16) | 512;
        uint32_t cmd = write ? (n > 1 ? CMD_WRITE_MULTI : CMD_WRITE_SINGLE)
                             : (n > 1 ? CMD_READ_MULTI : CMD_READ_SINGLE);
        if (!command(cmd, addr, NULL))
            return false;
        for (uint32_t b = 0; b < n; b++) {
            uint32_t r = wait_int(write ? INT_WRITE_RDY : INT_READ_RDY, 1000000);
            if (!r || (r & INT_ERROR)) {
                reset_lines();
                return false;
            }
            if (write) {
                for (int i = 0; i < 512; i += 4)
                    REG(EMMC_DATA) = (uint32_t)buf[i] | (uint32_t)buf[i + 1] << 8 |
                                     (uint32_t)buf[i + 2] << 16 | (uint32_t)buf[i + 3] << 24;
            } else {
                for (int i = 0; i < 512; i += 4) {
                    uint32_t w = REG(EMMC_DATA);
                    buf[i] = (uint8_t)w;
                    buf[i + 1] = (uint8_t)(w >> 8);
                    buf[i + 2] = (uint8_t)(w >> 16);
                    buf[i + 3] = (uint8_t)(w >> 24);
                }
            }
            buf += 512;
        }
        uint32_t r = wait_int(INT_DATA_DONE, 2000000);  // a write's programming time
        if (!r || (r & INT_ERROR)) {
            reset_lines();
            return false;
        }
        sector += n;
        count -= n;
    }
    return true;
}

bool emmc_read(uint8_t *buf, uint32_t sector, uint32_t count) {
    return transfer(false, buf, sector, count);
}

bool emmc_write(const uint8_t *buf, uint32_t sector, uint32_t count) {
    return transfer(true, (uint8_t *)buf, sector, count);
}
