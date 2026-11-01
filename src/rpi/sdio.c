/**
 * @file sdio.c
 * @brief SDIO on the BCM2835's EMMC controller (an Arasan SDHCI), routed to
 * GPIO34-39 where the Zero W's Wi-Fi chip is. Polled, data moved by the CPU.
 *
 * The command flags and the controller's habits (a short pause before each
 * register write, the clock divider) follow Richard Miller's bcm2835 emmc.c
 * for Plan 9, as Circle carries it.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdio.h"

#include "rpi.h"

#define EMMC_BASE (PERI_BASE + 0x300000)
#define BLKSIZECNT (EMMC_BASE + 0x04)
#define ARG1 (EMMC_BASE + 0x08)
#define CMDTM (EMMC_BASE + 0x0C)
#define RESP0 (EMMC_BASE + 0x10)
#define DATA (EMMC_BASE + 0x20)
#define STATUS (EMMC_BASE + 0x24)
#define CONTROL0 (EMMC_BASE + 0x28)
#define CONTROL1 (EMMC_BASE + 0x2C)
#define INTERRUPT (EMMC_BASE + 0x30)
#define IRPT_MASK (EMMC_BASE + 0x34)
#define IRPT_EN (EMMC_BASE + 0x38)

#define C0_DWIDTH4 (1u << 1)
#define C1_SRST_DATA (1u << 26)
#define C1_SRST_CMD (1u << 25)
#define C1_SRST_HC (1u << 24)
#define C1_CLK_EN (1u << 2)
#define C1_CLK_STABLE (1u << 1)
#define C1_CLK_INTLEN (1u << 0)
#define DTO 14u  ///< data timeout exponent

#define ISDATA (1u << 21)
#define IXCHK (1u << 20)
#define CRCCHK (1u << 19)
#define RESP_NONE (0u << 16)
#define RESP_48 (2u << 16)
#define RESP_48BUSY (3u << 16)
#define RESP_MASK (3u << 16)
#define MULTIBLOCK (1u << 5)
#define CARD2HOST (1u << 4)
#define BLKCNT_EN (1u << 1)

#define INT_ERR (1u << 15)
#define INT_CARD (1u << 8)
#define INT_READ_RDY (1u << 5)
#define INT_WRITE_RDY (1u << 4)
#define INT_DATA_DONE (1u << 1)
#define INT_CMD_DONE (1u << 0)

#define ST_DAT_INHIBIT (1u << 1)
#define ST_CMD_INHIBIT (1u << 0)

static uint32_t base_hz;
static bool fast;

/// A register write, after the pause the controller needs between them.
static void wr(uint32_t reg, uint32_t v) {
    timer_delay_us(fast ? 2 : 20);
    dmb();
    REG(reg) = v;
}

static uint32_t flags_for(unsigned cmd) {
    switch (cmd) {
    case 0: return RESP_NONE | IXCHK;
    case 3: return RESP_48 | IXCHK | CRCCHK;
    case 5: return RESP_48;
    case 7: return RESP_48BUSY | IXCHK | CRCCHK;
    case 52: return RESP_48 | IXCHK | CRCCHK;
    case 53: return RESP_48 | IXCHK | CRCCHK | ISDATA;
    default: return RESP_48;
    }
}

void sdio_clock(uint32_t hz) {
    uint32_t div = base_hz / (hz << 1);
    if (base_hz / (div << 1) > hz)
        div++;
    if (div > 0x3FF)
        div = 0x3FF;
    uint32_t c1 = ((div & 0xFF) << 8) | (((div >> 8) & 3) << 6);
    wr(CONTROL1, c1 | DTO << 16 | C1_CLK_EN | C1_CLK_INTLEN);
    uint64_t end = timer_us() + 1000000;
    while (!(REG(CONTROL1) & C1_CLK_STABLE) && timer_us() < end)
        ;
    fast = hz > 1000000;
}

void sdio_bus_width4(bool on) {
    wr(CONTROL0, on ? REG(CONTROL0) | C0_DWIDTH4 : REG(CONTROL0) & ~C0_DWIDTH4);
}

bool sdio_init(void) {
    // EMMC to the Wi-Fi chip: GPIO34 its clock (no pull), 35-39 CMD and DAT0-3.
    for (int pin = 34; pin <= 39; pin++) {
        gpio_function(pin, GPIO_ALT3);
        gpio_pull(pin, pin == 34 ? GPIO_PULL_NONE : GPIO_PULL_UP);
    }
    base_hz = mbox_clock_rate(1);
    if (!base_hz)
        base_hz = 100000000;
    fast = false;
    wr(CONTROL1, C1_SRST_HC);
    timer_delay_us(10000);
    uint64_t end = timer_us() + 100000;
    while (REG(CONTROL1) & C1_SRST_HC)
        if (timer_us() > end)
            return false;
    wr(CONTROL1, C1_SRST_DATA);
    timer_delay_us(10000);
    wr(CONTROL1, 0);
    sdio_clock(400000);
    wr(IRPT_EN, 0);  // flags only: polled
    wr(IRPT_MASK, ~0u);
    wr(INTERRUPT, ~0u);
    return true;
}

/// Resets the command (and data) circuits after an error.
static void reset_lines(bool data) {
    uint32_t bits = C1_SRST_CMD | (data ? C1_SRST_DATA : 0);
    wr(CONTROL1, REG(CONTROL1) | bits);
    uint64_t end = timer_us() + 100000;
    while ((REG(CONTROL1) & bits) && timer_us() < end)
        ;
}

bool sdio_cmd(unsigned cmd, uint32_t arg, uint32_t *resp) {
    uint32_t c = (uint32_t)cmd << 24 | flags_for(cmd);
    if (cmd == 53) {
        c |= arg & (1u << 31) ? 0 : CARD2HOST;
        if ((REG(BLKSIZECNT) & 0xFFFF0000u) != 0x10000u)
            c |= MULTIBLOCK | BLKCNT_EN;
    }
    if (REG(STATUS) & ST_CMD_INHIBIT)
        reset_lines(false);
    if ((REG(STATUS) & ST_DAT_INHIBIT) && ((c & ISDATA) || (c & RESP_MASK) == RESP_48BUSY))
        reset_lines(true);
    wr(ARG1, arg);
    uint32_t i = REG(INTERRUPT) & ~INT_CARD;
    if (i)
        wr(INTERRUPT, i);
    wr(CMDTM, c);
    uint64_t end = timer_us() + 1000000;
    while (!((i = REG(INTERRUPT)) & (INT_CMD_DONE | INT_ERR)))
        if (timer_us() > end)
            break;
    if ((i & (INT_CMD_DONE | INT_ERR)) != INT_CMD_DONE) {
        wr(INTERRUPT, i & ~INT_CARD);
        reset_lines(false);
        return false;
    }
    wr(INTERRUPT, i & ~(INT_DATA_DONE | INT_READ_RDY | INT_WRITE_RDY | INT_CARD));
    if (resp)
        *resp = (c & RESP_MASK) == RESP_NONE ? 0 : REG(RESP0);
    if ((c & RESP_MASK) == RESP_48BUSY) {
        end = timer_us() + 3000000;
        while (!((i = REG(INTERRUPT)) & (INT_DATA_DONE | INT_ERR)) && timer_us() < end)
            ;
        wr(INTERRUPT, i & ~INT_CARD);
    }
    return true;
}

bool sdio_io(bool write, uint8_t *buf, unsigned block, unsigned count) {
    uint32_t mask = write ? INT_WRITE_RDY : INT_READ_RDY;
    for (unsigned b = 0; b < count; b++) {
        uint64_t end = timer_us() + 1000000;
        uint32_t i;
        while (!((i = REG(INTERRUPT)) & (mask | INT_ERR)))
            if (timer_us() > end) {
                reset_lines(true);
                return false;
            }
        if (i & INT_ERR) {
            wr(INTERRUPT, i & ~INT_CARD);
            reset_lines(true);
            return false;
        }
        REG(INTERRUPT) = i & mask;
        unsigned words = (block + 3) / 4;
        for (unsigned w = 0; w < words; w++, buf += 4) {
            if (write)
                REG(DATA) = (uint32_t)buf[0] | (uint32_t)buf[1] << 8 | (uint32_t)buf[2] << 16 |
                            (uint32_t)buf[3] << 24;
            else {
                uint32_t v = REG(DATA);
                buf[0] = (uint8_t)v;
                buf[1] = (uint8_t)(v >> 8);
                buf[2] = (uint8_t)(v >> 16);
                buf[3] = (uint8_t)(v >> 24);
            }
        }
        buf += block - words * 4;  // (block is a multiple of 4 here)
    }
    uint64_t end = timer_us() + 3000000;
    uint32_t i;
    while (!((i = REG(INTERRUPT)) & (INT_DATA_DONE | INT_ERR)))
        if (timer_us() > end) {
            reset_lines(true);
            return false;
        }
    wr(INTERRUPT, i & ~INT_CARD);
    if (i & INT_ERR) {
        reset_lines(true);
        return false;
    }
    return true;
}

void sdio_setup(unsigned block, unsigned count) {
    wr(BLKSIZECNT, count << 16 | block);
}

bool sdio_card_irq(void) {
    if (!(REG(INTERRUPT) & INT_CARD))
        return false;
    wr(INTERRUPT, INT_CARD);
    return true;
}
