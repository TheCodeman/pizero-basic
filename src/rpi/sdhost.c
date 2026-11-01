/**
 * @file sdhost.c
 * @brief The SD card in the Zero's slot, through the BCM2835's own SD host
 * controller (SDHOST, at 0x20202000) on GPIO48-53: SD and SDHC/SDXC cards,
 * 4-bit bus at up to 25 MHz, data moved by the CPU. This leaves the EMMC
 * controller for the Wi-Fi chip (on GPIO34-39), as Linux and Circle do.
 *
 * Polled. The registers and their quirks (the FIFO thresholds, the slow-card
 * clock, the state machine after a transfer) are those of Linux's
 * bcm2835-sdhost.c (Phil Elwell, Raspberry Pi, GPL-2.0) as ported to Circle.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sdcard.h"

#include "rpi.h"

#define SDHOST_BASE (PERI_BASE + 0x202000)
#define SDCMD (SDHOST_BASE + 0x00)
#define SDARG (SDHOST_BASE + 0x04)
#define SDTOUT (SDHOST_BASE + 0x08)
#define SDCDIV (SDHOST_BASE + 0x0C)
#define SDRSP0 (SDHOST_BASE + 0x10)
#define SDHSTS (SDHOST_BASE + 0x20)
#define SDVDD (SDHOST_BASE + 0x30)
#define SDEDM (SDHOST_BASE + 0x34)
#define SDHCFG (SDHOST_BASE + 0x38)
#define SDHBCT (SDHOST_BASE + 0x3C)
#define SDDATA (SDHOST_BASE + 0x40)
#define SDHBLC (SDHOST_BASE + 0x50)

#define SDCMD_NEW (1u << 15)
#define SDCMD_FAIL (1u << 14)
#define SDCMD_BUSYWAIT (1u << 11)
#define SDCMD_NO_RESPONSE (1u << 10)
#define SDCMD_LONG_RESPONSE (1u << 9)
#define SDCMD_WRITE (1u << 7)
#define SDCMD_READ (1u << 6)

#define HSTS_BUSY_IRPT 0x400
#define HSTS_REW_TIME_OUT 0x80
#define HSTS_CMD_TIME_OUT 0x40
#define HSTS_CRC16_ERROR 0x20
#define HSTS_CRC7_ERROR 0x10
#define HSTS_FIFO_ERROR 0x08
#define HSTS_ERROR_MASK (HSTS_CMD_TIME_OUT | HSTS_CRC16_ERROR | HSTS_CRC7_ERROR | \
                         HSTS_REW_TIME_OUT | HSTS_FIFO_ERROR)

#define HCFG_BUSY_IRPT_EN (1u << 10)
#define HCFG_SLOW_CARD (1u << 3)
#define HCFG_WIDE_EXT_BUS (1u << 2)
#define HCFG_WIDE_INT_BUS (1u << 1)

#define EDM_FORCE_DATA_MODE (1u << 19)
#define EDM_FSM_MASK 0xF
#define EDM_FSM_IDENTMODE 0x0
#define EDM_FSM_DATAMODE 0x1
#define EDM_FSM_READWAIT 0x4
#define EDM_FSM_WRITESTART1 0xA
#define EDM_FIFO_COUNT(edm) (((edm) >> 4) & 0x1F)
#define FIFO_WORDS 16

/// A command's response: none, 48 bits, 48 with busy, 136 bits.
enum { RSP_NONE, RSP_48, RSP_48B, RSP_136 };

static struct {
    bool ready;
    bool sdhc;         ///< block addressed (SDHC/SDXC)
    uint32_t rca;      ///< relative address (top 16 bits)
    uint32_t core_hz;  ///< SDHOST's clock: the core clock
    uint32_t sectors;
    uint32_t hcfg;
    uint32_t resp[4];  ///< the last response, resp[0] bits 31:0
} card;

/// Sets the card's clock to at most `hz`.
static void set_clock(uint32_t hz) {
    uint32_t div = card.core_hz / hz;
    if (div < 2)
        div = 2;
    if (card.core_hz / div > hz)
        div++;
    div -= 2;
    if (div > 0x7FF)
        div = 0x7FF;
    REG(SDCDIV) = div;
}

/// Sends a command and waits for it: false if it failed.
static bool command(unsigned index, uint32_t arg, int rsp, uint32_t data_flags) {
    uint64_t end = timer_us() + 100000;
    while (REG(SDCMD) & SDCMD_NEW)
        if (timer_us() > end)
            return false;
    uint32_t hsts = REG(SDHSTS);
    if (hsts & (HSTS_ERROR_MASK | HSTS_BUSY_IRPT))
        REG(SDHSTS) = hsts;
    uint32_t cmd = (index & 0x3F) | data_flags;
    if (rsp == RSP_NONE)
        cmd |= SDCMD_NO_RESPONSE;
    else if (rsp == RSP_136)
        cmd |= SDCMD_LONG_RESPONSE;
    else if (rsp == RSP_48B)
        cmd |= SDCMD_BUSYWAIT;
    REG(SDARG) = arg;
    REG(SDCMD) = cmd | SDCMD_NEW;
    end = timer_us() + 500000;
    uint32_t c;
    while ((c = REG(SDCMD)) & SDCMD_NEW)
        if (timer_us() > end)
            return false;
    if (c & SDCMD_FAIL) {
        hsts = REG(SDHSTS);
        REG(SDHSTS) = HSTS_ERROR_MASK;
        uint32_t edm = REG(SDEDM);
        uint32_t fsm = edm & EDM_FSM_MASK;
        if (fsm == EDM_FSM_READWAIT || fsm == EDM_FSM_WRITESTART1)
            REG(SDEDM) = edm | EDM_FORCE_DATA_MODE;
        return false;
    }
    if (rsp == RSP_136)
        for (int i = 0; i < 4; i++)
            card.resp[i] = REG(SDRSP0 + 4 * i);
    else if (rsp != RSP_NONE)
        card.resp[0] = REG(SDRSP0);
    if (rsp == RSP_48B) {
        // The card holds DAT0 low while busy; the controller says when it's done.
        end = timer_us() + 1000000;
        while (!(REG(SDHSTS) & HSTS_BUSY_IRPT) && timer_us() < end)
            ;
        REG(SDHSTS) = HSTS_BUSY_IRPT;
    }
    return true;
}

static bool app_command(unsigned index, uint32_t arg, int rsp) {
    return command(55, card.rca, RSP_48, 0) && command(index, arg, rsp, 0);
}

/// Waits until the card is back in the transfer state, ready for data
/// (after a write it is programming its flash).
static bool wait_ready(void) {
    uint64_t end = timer_us() + 2000000;
    do {
        if (command(13, card.rca, RSP_48, 0) && (card.resp[0] & (1u << 8)) &&
            ((card.resp[0] >> 9) & 0xF) == 4)
            return true;
    } while (timer_us() < end);
    return false;
}

/// After a data transfer: the controller's state machine back to idle.
static void wait_transfer_complete(bool read) {
    uint32_t alternate_idle = read ? EDM_FSM_READWAIT : EDM_FSM_WRITESTART1;
    for (int i = 0; i < 100000; i++) {
        uint32_t edm = REG(SDEDM);
        uint32_t fsm = edm & EDM_FSM_MASK;
        if (fsm == EDM_FSM_IDENTMODE || fsm == EDM_FSM_DATAMODE)
            return;
        if (fsm == alternate_idle) {
            REG(SDEDM) = edm | EDM_FORCE_DATA_MODE;
            return;
        }
    }
}

/// Bits hi..lo of the 128-bit CSD (resp[0] bits 31:0).
static uint32_t csd_bits(int hi, int lo) {
    uint32_t v = 0;
    for (int b = hi; b >= lo; b--)
        v = (v << 1) | ((card.resp[b / 32] >> (b % 32)) & 1);
    return v;
}

static void read_size(void) {
    if (csd_bits(127, 126) == 1) {  // CSD 2.0 (SDHC, SDXC)
        card.sectors = (csd_bits(69, 48) + 1) * 1024;
    } else {  // CSD 1.0
        uint32_t c_size = csd_bits(73, 62), mult = csd_bits(49, 47), bl_len = csd_bits(83, 80);
        uint64_t bytes = ((uint64_t)(c_size + 1) << (mult + 2)) << bl_len;
        card.sectors = (uint32_t)(bytes / 512);
    }
}

const char *sdcard_init(void) {
    card.ready = false;
    card.rca = 0;
    card.core_hz = mbox_clock_rate(4);
    if (!card.core_hz)
        card.core_hz = 250000000;

    // The card's pins to SDHOST (ALT0): CLK without a pull, CMD and DAT0-3
    // pulled up.
    for (int pin = 48; pin <= 53; pin++) {
        gpio_function(pin, GPIO_ALT0);
        gpio_pull(pin, pin == 48 ? GPIO_PULL_NONE : GPIO_PULL_UP);
    }

    // Reset, as Linux does.
    REG(SDVDD) = 0;
    REG(SDCMD) = 0;
    REG(SDARG) = 0;
    REG(SDTOUT) = 0xF00000;
    REG(SDCDIV) = 0;
    REG(SDHSTS) = 0x7F8;
    REG(SDHCFG) = 0;
    REG(SDHBCT) = 0;
    REG(SDHBLC) = 0;
    uint32_t edm = REG(SDEDM);  // the FIFO thresholds, for a silicon bug
    edm &= ~((0x1Fu << 14) | (0x1Fu << 9));
    edm |= (4u << 14) | (4u << 9);
    REG(SDEDM) = edm;
    timer_delay_us(10000);
    REG(SDVDD) = 1;
    timer_delay_us(10000);
    // The slow-card bit keeps the clock divider in use in data mode too
    // (the core clock is above 250 MHz with force_turbo).
    card.hcfg = HCFG_BUSY_IRPT_EN | HCFG_WIDE_INT_BUS | HCFG_SLOW_CARD;
    REG(SDHCFG) = card.hcfg;
    set_clock(400000);
    timer_delay_us(1000);  // 74 clocks before the first command

    command(0, 0, RSP_NONE, 0);
    bool v2 = command(8, 0x1AA, RSP_48, 0) && (card.resp[0] & 0xFFF) == 0x1AA;

    uint64_t end = timer_us() + 1000000;
    for (;;) {
        if (!app_command(41, 0x00FF8000u | (v2 ? 0x40000000u : 0), RSP_48))
            return "none";
        if (card.resp[0] == 0)
            return "none";  // SDHOST says nothing when no card answers
        if (card.resp[0] & 0x80000000u)
            break;
        if (timer_us() > end)
            return "the card didn't power up";
        timer_delay_us(10000);
    }
    card.sdhc = (card.resp[0] & 0x40000000u) != 0;

    if (!command(2, 0, RSP_136, 0))
        return "no CID";
    if (!command(3, 0, RSP_48, 0))
        return "no address";
    card.rca = card.resp[0] & 0xFFFF0000u;
    if (!command(9, card.rca, RSP_136, 0))
        return "no CSD";
    read_size();
    if (!command(7, card.rca, RSP_48B, 0))
        return "it can't be selected";
    if (!card.sdhc && !command(16, 512, RSP_48, 0))
        return "it won't take 512-byte blocks";

    // 4-bit bus, then full speed.
    if (app_command(6, 2, RSP_48)) {
        card.hcfg |= HCFG_WIDE_EXT_BUS;
        REG(SDHCFG) = card.hcfg;
    }
    set_clock(25000000);
    card.ready = true;
    return NULL;
}

bool sdcard_ready(void) {
    return card.ready;
}

uint32_t sdcard_sectors(void) {
    return card.sectors;
}

/// Moves the words of `count` blocks through the FIFO; false on an error.
static bool move_data(bool write, uint8_t *buf, uint32_t count) {
    uint32_t words = count * 128;
    uint64_t end = timer_us() + 500000 + (uint64_t)count * 10000;
    while (words) {
        uint32_t edm = REG(SDEDM);
        uint32_t n = write ? FIFO_WORDS - EDM_FIFO_COUNT(edm) : EDM_FIFO_COUNT(edm);
        if (n == 0 || n > FIFO_WORDS) {
            if (REG(SDHSTS) & HSTS_ERROR_MASK)
                return false;
            if (timer_us() > end)
                return false;
            continue;
        }
        if (n > words)
            n = words;
        words -= n;
        while (n--) {
            if (write) {
                REG(SDDATA) = (uint32_t)buf[0] | (uint32_t)buf[1] << 8 | (uint32_t)buf[2] << 16 |
                              (uint32_t)buf[3] << 24;
            } else {
                uint32_t w = REG(SDDATA);
                buf[0] = (uint8_t)w;
                buf[1] = (uint8_t)(w >> 8);
                buf[2] = (uint8_t)(w >> 16);
                buf[3] = (uint8_t)(w >> 24);
            }
            buf += 4;
        }
    }
    return !(REG(SDHSTS) & HSTS_ERROR_MASK);
}

static bool transfer(bool write, uint8_t *buf, uint32_t sector, uint32_t count) {
    if (!card.ready)
        return false;
    while (count) {
        uint32_t n = count > 256 ? 256 : count;  // SDHBLC is 9 bits
        uint32_t addr = card.sdhc ? sector : sector * 512;
        REG(SDHBCT) = 512;
        REG(SDHBLC) = n;
        unsigned index = write ? (n > 1 ? 25 : 24) : (n > 1 ? 18 : 17);
        bool ok = command(index, addr, RSP_48, write ? SDCMD_WRITE : SDCMD_READ) &&
                  move_data(write, buf, n);
        if (n > 1 || !ok) {
            wait_transfer_complete(!write);
            command(12, 0, RSP_48B, 0);  // stop the transfer (and after an error)
        } else {
            wait_transfer_complete(!write);
        }
        REG(SDHSTS) = HSTS_ERROR_MASK;
        if (!wait_ready() || !ok)
            return false;
        buf += n * 512;
        sector += n;
        count -= n;
    }
    return true;
}

bool sdcard_read(uint8_t *buf, uint32_t sector, uint32_t count) {
    return transfer(false, buf, sector, count);
}

bool sdcard_write(const uint8_t *buf, uint32_t sector, uint32_t count) {
    return transfer(true, (uint8_t *)buf, sector, count);
}
