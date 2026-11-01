/**
 * @file wlan.c
 * @brief The Zero W's Wi-Fi chip, a Cypress CYW43430 (Broadcom 43430), over
 * SDIO (sdio.c). Polled: there is no interrupt handler, the caller waits
 * while the chip works.
 *
 * The chip's bring-up (the Silicon Backplane window, the core scan, the
 * firmware and NVRAM upload, the HT clock), its SDPCM framing and its
 * command and event formats follow Richard Miller's ether4330.c for Plan 9
 * (the bcm port, as Circle's addon/wlan carries it).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "wlan.h"

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "rpi.h"
#include "sdio.h"

enum {
    Fn0 = 0, Fn1 = 1, Fn2 = 2,
    Fbr1 = 0x100, Fbr2 = 0x200,

    // CCCR
    Ioenable = 0x02, Ioready = 0x03, Intenable = 0x04, Intpend = 0x05, Ioabort = 0x06,
    Busifc = 0x07, Blksize = 0x10, Highspeed = 0x13,

    // SDIO commands
    GO_IDLE_STATE = 0, SEND_RELATIVE_ADDR = 3, IO_SEND_OP_COND = 5, SELECT_CARD = 7,
    IO_RW_DIRECT = 52, IO_RW_EXTENDED = 53,
    V3_3 = 3 << 20,

    // The Silicon Backplane
    Sbwsize = 0x8000, Sb32bit = 0x8000,
    Sbaddr = 0x1000a,
    Enumbase = 0x18000000,
    Framectl = 0x1000d, Rfhalt = 0x01, Wfhalt = 0x02,
    Clkcsr = 0x1000e,
    ForceALP = 0x01, ForceHT = 0x02, ReqALP = 0x08, ReqHT = 0x10, Nohwreq = 0x20,
    ALPavail = 0x40, HTavail = 0x80,
    Pullups = 0x1000f,
    Wfrmcnt = 0x10019, Rfrmcnt = 0x1001b,

    // core control
    Ioctrl = 0x408, Resetctrl = 0x800,
    // socram
    Coreinfo = 0x00, Bankidx = 0x10, Bankinfo = 0x40, Bankpda = 0x44,
    // chipcommon
    Gpiopullup = 0x58, Gpiopulldown = 0x5c,
    // the SDIO core
    Intstatus = 0x20, Fcchange = 1 << 5, FrameInt = 1 << 6, MailboxInt = 1 << 7,
    Intmask = 0x24, Sbmbox = 0x40, Sbmboxdata = 0x48, Hostmboxdata = 0x4c,

    ARMcm3 = 0x82A, ARM7tdmi = 0x825, ARMcr4 = 0x83E,

    GetVar = 262, SetVar = 263,

    SDPCM = 12,  ///< the frame header
    CDC = 16,    ///< a command's header
    PKT = 2048,  ///< the largest frame
};

static jmp_buf fail_jump;
static const char *fail_why;

/// Gives up on what the chip was doing.
static void fail(const char *why) {
    fail_why = why;
    longjmp(fail_jump, 1);
}

static struct {
    bool up;
    int chipid, chiprev, armcore;
    uint32_t chipcommon, armctl, armregs, d11ctl, socramregs, socramctl, sdregs;
    int socramrev;
    uint32_t socramsize, rambase;
    uint32_t window;  ///< the backplane window now (~0: unknown)
    uint16_t reqid;
    uint8_t txseq, txwindow, fcmask;
    uint8_t mac[6];
    char version[128];
} chip;

static uint8_t txbuf[PKT + 64], rxbuf[PKT + 64];
static uint8_t *rsp;  ///< the awaited command's response (in rxbuf), once it comes
static int rsp_len;
static bool rsp_ready;  ///< rsp has come: stop reading frames

static wlan_found_t *scan_found;
static bool scan_done;

static wlan_rx_t *rx_handler;
static bool busy;  ///< inside one of the interface's calls: wlan_poll waits

/// The join's progress, from the firmware's events.
enum { J_ASSOC = 1, J_LINK = 2, J_KEYED = 4 };
static struct {
    bool active;   ///< joining or joined
    bool secure;   ///< WPA2: wait for the keys too
    uint8_t bits;  ///< J_*
    int failed;    ///< 0, or WLAN_FAIL, WLAN_NONET, WLAN_BADAUTH
} join;

static void put2(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8);
}
static void put4(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}
static uint32_t get2(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8;
}
static uint32_t get4(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t getbe2(const uint8_t *p) {
    return (uint32_t)p[0] << 8 | p[1];
}
static uint32_t getbe4(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// ---- SDIO --------------------------------------------------------------------------

static uint32_t cmd(unsigned c, uint32_t arg) {
    uint32_t r = 0;
    if (!sdio_cmd(c, arg, &r))
        fail("the Wi-Fi chip doesn't answer");
    return r;
}

static int rd(int fn, uint32_t addr) {
    uint32_t r = cmd(IO_RW_DIRECT, (uint32_t)(fn & 7) << 28 | (addr & 0x1FFFF) << 9);
    if (r & 0xCF00)
        fail("Wi-Fi chip register read failed");
    return r & 0xFF;
}

static void wrb(int fn, uint32_t addr, int data) {
    for (int retry = 0; retry < 10; retry++) {
        uint32_t r = 0;
        if (sdio_cmd(IO_RW_DIRECT, 1u << 31 | (uint32_t)(fn & 7) << 28 | (addr & 0x1FFFF) << 9 | (data & 0xFF), &r) &&
            !(r & 0xCF00))
            return;
    }
    fail("Wi-Fi chip register write failed");
}

static void setbits(int fn, uint32_t addr, int bits) {
    wrb(fn, addr, rd(fn, addr) | bits);
}

/// CMD53: len bytes (a multiple of 4 unless under one block).
static bool xfer(int fn, bool write, uint8_t *buf, int len, uint32_t addr, bool incr) {
    int bsize = fn == Fn2 ? 512 : 64;
    while (len > 0) {
        int blk, bcount, m;
        if (len >= 511 * bsize)
            blk = 1, bcount = 511, m = bcount * bsize;
        else if (len > bsize)
            blk = 1, bcount = len / bsize, m = bcount * bsize;
        else
            blk = 0, bcount = len, m = bcount;
        if (blk)
            sdio_setup((unsigned)bsize, (unsigned)bcount);
        else
            sdio_setup((unsigned)bcount, 1);
        uint32_t arg = (uint32_t)write << 31 | (uint32_t)(fn & 7) << 28 | (uint32_t)blk << 27 |
                       (uint32_t)incr << 26 | (addr & 0x1FFFF) << 9 | (bcount & 0x1FF);
        uint32_t r;
        if (!sdio_cmd(IO_RW_EXTENDED, arg, &r))
            return false;
        if (!sdio_io(write, buf, blk ? (unsigned)bsize : (unsigned)bcount, blk ? (unsigned)bcount : 1))
            return false;
        len -= m;
        buf += m;
        if (incr)
            addr += (uint32_t)m;
    }
    return true;
}

static void abort_fn(int fn) {
    uint32_t r;
    sdio_cmd(IO_RW_DIRECT, 1u << 31 | (uint32_t)Ioabort << 9 | (fn & 7), &r);
}

static void ext(int fn, bool write, uint8_t *buf, int len, uint32_t addr, bool incr) {
    if (!xfer(fn, write, buf, len, addr, incr))
        fail("Wi-Fi chip transfer failed");
}

static uint32_t cfgreadl(uint32_t off) {
    uint8_t b[4] = {0};
    ext(Fn1, false, b, 4, off | Sb32bit, true);
    return get4(b);
}

static void cfgwritel(uint32_t off, uint32_t v) {
    uint8_t b[4];
    for (int retry = 0; retry < 3; retry++) {
        put4(b, v);
        if (xfer(Fn1, true, b, 4, off | Sb32bit, true))
            return;
        abort_fn(Fn1);
    }
    fail("Wi-Fi chip write failed");
}

static void cfgw(uint32_t off, int v) {
    wrb(Fn1, off, v);
}
static int cfgr(uint32_t off) {
    return rd(Fn1, off);
}

static void sbwindow(uint32_t addr) {
    addr &= ~(uint32_t)(Sbwsize - 1);
    if (addr == chip.window)
        return;
    cfgw(Sbaddr, (int)(addr >> 8));
    cfgw(Sbaddr + 1, (int)(addr >> 16));
    cfgw(Sbaddr + 2, (int)(addr >> 24));
    chip.window = addr;
}

static void sbrw(bool write, uint8_t *buf, int len, uint32_t off) {
    if (len >= 4) {
        int n = len & ~3;
        ext(Fn1, write, buf, n, off | Sb32bit, true);
        off += (uint32_t)n, buf += n, len -= n;
    }
    for (; len > 0; off++, buf++, len--) {
        if (write)
            wrb(Fn1, off | Sb32bit, *buf);
        else
            *buf = (uint8_t)rd(Fn1, off | Sb32bit);
    }
}

/// Reads or writes the chip's memory through the backplane window.
static void sbmem(bool write, uint8_t *buf, int len, uint32_t off) {
    uint32_t n = ((off + Sbwsize - 1) & ~(uint32_t)(Sbwsize - 1)) - off;
    if (n == 0)
        n = Sbwsize;
    while (len > 0) {
        if (n > (uint32_t)len)
            n = (uint32_t)len;
        sbwindow(off);
        sbrw(write, buf, (int)n, off & (Sbwsize - 1));
        off += n, buf += n, len -= (int)n;
        n = Sbwsize;
    }
}

/// Moves a frame on function 2 (len rounded up to 4; buf has room).
static void packetrw(bool write, uint8_t *buf, int len) {
    len = (len + 3) & ~3;
    for (int retry = 0;; retry++) {
        if (xfer(Fn2, write, buf, len, Enumbase, false))
            return;
        abort_fn(Fn2);
        if (retry == 2)
            fail("Wi-Fi frame transfer failed");
    }
}

// ---- The chip's cores --------------------------------------------------------------

static void sbdisable(uint32_t regs, uint32_t pre, uint32_t ioctl) {
    sbwindow(regs);
    if (cfgreadl(regs + Resetctrl) & 1) {
        cfgwritel(regs + Ioctrl, 3 | ioctl);
        cfgreadl(regs + Ioctrl);
        return;
    }
    cfgwritel(regs + Ioctrl, 3 | pre);
    cfgreadl(regs + Ioctrl);
    cfgwritel(regs + Resetctrl, 1);
    timer_delay_us(10);
    uint64_t end = timer_us() + 100000;
    while (!(cfgreadl(regs + Resetctrl) & 1))
        if (timer_us() > end)
            fail("Wi-Fi chip core won't reset");
    cfgwritel(regs + Ioctrl, 3 | ioctl);
    cfgreadl(regs + Ioctrl);
}

static void sbreset(uint32_t regs, uint32_t pre, uint32_t ioctl) {
    sbdisable(regs, pre, ioctl);
    sbwindow(regs);
    uint64_t end = timer_us() + 100000;
    while (cfgreadl(regs + Resetctrl) & 1) {
        cfgwritel(regs + Resetctrl, 0);
        timer_delay_us(40);
        if (timer_us() > end)
            fail("Wi-Fi chip core won't start");
    }
    cfgwritel(regs + Ioctrl, 1 | ioctl);
    cfgreadl(regs + Ioctrl);
}

static void corescan(uint32_t r) {
    static uint8_t buf[512];
    sbmem(false, buf, sizeof buf, r);
    int coreid = 0, corerev = 0;
    for (unsigned i = 0; i < sizeof buf; i += 4) {
        switch (buf[i] & 0xF) {
        case 0xF:  // the end
            return;
        case 0x1:  // a core
            if (i + 4 >= sizeof buf || (buf[i + 4] & 0xF) != 0x1)
                break;
            coreid = (buf[i + 1] | buf[i + 2] << 8) & 0xFFF;
            i += 4;
            corerev = buf[i + 3];
            break;
        case 0x5: {  // an address
            uint32_t addr = (uint32_t)buf[i + 1] << 8 | (uint32_t)buf[i + 2] << 16 | (uint32_t)buf[i + 3] << 24;
            addr &= ~0xFFFu;
            bool ctl = buf[i] & 0xC0;
            switch (coreid) {
            case 0x800:
                if (!ctl)
                    chip.chipcommon = addr;
                break;
            case ARMcm3: case ARM7tdmi: case ARMcr4:
                chip.armcore = coreid;
                if (ctl) {
                    if (!chip.armctl)
                        chip.armctl = addr;
                } else if (!chip.armregs) {
                    chip.armregs = addr;
                }
                break;
            case 0x80E:
                if (ctl)
                    chip.socramctl = addr;
                else if (!chip.socramregs)
                    chip.socramregs = addr;
                chip.socramrev = corerev;
                break;
            case 0x829:
                if (!ctl)
                    chip.sdregs = addr;
                break;
            case 0x812:
                if (ctl)
                    chip.d11ctl = addr;
                break;
            }
            break;
        }
        }
    }
}

static void ramscan(void) {
    if (chip.armcore == ARMcr4)
        fail("this Wi-Fi chip isn't supported");  // the Zero W's is an ARM Cortex-M3
    if (chip.socramrev <= 7 || chip.socramrev == 12)
        fail("this Wi-Fi chip's RAM isn't supported");
    sbreset(chip.socramctl, 0, 0);
    uint32_t r = chip.socramregs;
    sbwindow(r);
    uint32_t n = cfgreadl(r + Coreinfo);
    int banks = (n >> 4) & 0xF;
    uint32_t size = 0;
    for (int i = 0; i < banks; i++) {
        cfgwritel(r + Bankidx, (uint32_t)i);
        n = cfgreadl(r + Bankinfo);
        size += 8192 * ((n & 0x3F) + 1);
    }
    chip.socramsize = size;
    chip.rambase = 0;
    if (chip.chipid == 43430) {
        cfgwritel(r + Bankidx, 3);
        cfgwritel(r + Bankpda, 0);
    }
}

static void wait_clock(int bits, unsigned ms) {
    uint64_t end = timer_us() + ms * 1000ull;
    while (!(cfgr(Clkcsr) & bits)) {
        if (timer_us() > end)
            fail("the Wi-Fi chip's clock won't start");
        timer_delay_us(10);
    }
}

static void sdioinit(void) {
    if (!sdio_init())
        fail("the SDIO controller won't reset");
    sdio_cmd(GO_IDLE_STATE, 0, NULL);
    uint32_t ocr = 0;
    sdio_cmd(IO_SEND_OP_COND, 0, &ocr);
    for (int i = 0; !(ocr & 1u << 31); i++) {
        if (i > 5)
            fail("no Wi-Fi chip answers");
        ocr = 0;
        sdio_cmd(IO_SEND_OP_COND, V3_3, &ocr);
        timer_delay_us(100000);
    }
    uint32_t rca = cmd(SEND_RELATIVE_ADDR, 0) >> 16;
    cmd(SELECT_CARD, rca << 16);
    timer_delay_us(1000);
    sdio_clock(25000000);
    timer_delay_us(1000);
    setbits(Fn0, Highspeed, 2);
    setbits(Fn0, Busifc, 2);  // 4 bits wide
    sdio_bus_width4(true);
    wrb(Fn0, Fbr1 + Blksize, 64);
    wrb(Fn0, Fbr1 + Blksize + 1, 64 >> 8);
    wrb(Fn0, Fbr2 + Blksize, 512 & 0xFF);
    wrb(Fn0, Fbr2 + Blksize + 1, 512 >> 8);
    setbits(Fn0, Ioenable, 1 << Fn1);
    wrb(Fn0, Intenable, 0);
    for (int i = 0; !(rd(Fn0, Ioready) & 1 << Fn1); i++) {
        if (i == 10)
            fail("can't enable the Wi-Fi chip's backplane");
        timer_delay_us(100000);
    }
}

static void sbinit(void) {
    chip.window = ~0u;
    sbwindow(Enumbase);
    uint32_t r = cfgreadl(Enumbase);
    chip.chipid = (int)(r & 0xFFFF);
    chip.chiprev = (int)((r >> 16) & 0xF);
    if (chip.chipid != 43430)
        fail("not a 43430 Wi-Fi chip");
    r = cfgreadl(Enumbase + 63 * 4);
    corescan(r);
    if (!chip.armctl || !chip.d11ctl || !chip.socramctl || !chip.socramregs || !chip.sdregs)
        fail("the Wi-Fi chip's cores aren't all there");
    sbdisable(chip.armctl, 0, 0);
    sbreset(chip.d11ctl, 8 | 4, 4);
    ramscan();
    cfgw(Clkcsr, 0);
    timer_delay_us(10);
    cfgw(Clkcsr, Nohwreq | ReqALP);
    wait_clock(HTavail | ALPavail, 100);
    cfgw(Clkcsr, Nohwreq | ForceALP);
    timer_delay_us(65);
    cfgw(Pullups, 0);
    sbwindow(chip.chipcommon);
    cfgwritel(chip.chipcommon + Gpiopullup, 0);
    cfgwritel(chip.chipcommon + Gpiopulldown, 0);
}

// ---- Firmware ----------------------------------------------------------------------

static const char *fw_name(const char *ext) {
    static char path[64];
    snprintf(path, sizeof path, WLAN_FIRMWARE_DIR "brcmfmac%s-sdio.%s", chip.chiprev == 2 ? "43436" : "43430", ext);
    return path;
}

static FIL fw_file;
static bool fw_open;

static void fw_close(void) {
    if (fw_open)
        f_close(&fw_file);
    fw_open = false;
}

static void fw_start(const char *ext) {
    fw_close();
    if (f_open(&fw_file, fw_name(ext), FA_READ) != FR_OK) {
        static char why[96];
        snprintf(why, sizeof why, "no %s", fw_name(ext));
        fail(why);
    }
    fw_open = true;
}

static int fw_read(uint8_t *buf, int n) {
    UINT got = 0;
    if (f_read(&fw_file, buf, (UINT)n, &got) != FR_OK)
        fail("can't read the Wi-Fi firmware");
    return (int)got;
}

/// The NVRAM text as the firmware wants it: "var=value\0" lines, two NULs, padded to 4.
static int condense(uint8_t *buf, int n) {
    uint8_t *op = buf, *lp = buf;
    bool skipping = false;
    for (uint8_t *p = buf; p < buf + n; p++) {
        int c = *p;
        if (c == '#') {
            skipping = true;
        } else if (c == '\0' || c == '\n') {
            skipping = false;
            if (op != lp)
                *op++ = '\0', lp = op;
        } else if (c != '\r' && !skipping) {
            *op++ = (uint8_t)c;
        }
    }
    if (!skipping && op != lp)
        *op++ = '\0';
    *op++ = '\0';
    while ((op - buf) & 3)
        *op++ = '\0';
    return (int)(op - buf);
}

static void fwload(void) {
    static uint8_t buf[2048], check[2048];
    cfgw(Clkcsr, ReqALP);
    wait_clock(ALPavail, 100);
    memset(buf, 0, 4);
    sbmem(true, buf, 4, chip.rambase + chip.socramsize - 4);

    // The firmware, then read back to check it.
    fw_start("bin");
    uint32_t off = 0;
    for (int n; (n = fw_read(buf, sizeof buf)) > 0; off += (uint32_t)n) {
        while (n & 3)
            buf[n++] = 0;
        sbmem(true, buf, n, chip.rambase + off);
    }
    if (off < 4096)
        fail("the Wi-Fi firmware file is too short");
    f_lseek(&fw_file, 0);
    off = 0;
    for (int n; (n = fw_read(buf, sizeof buf)) > 0; off += (uint32_t)n) {
        while (n & 3)
            buf[n++] = 0;
        sbmem(false, check, n, chip.rambase + off);
        if (memcmp(buf, check, (size_t)n))
            fail("the Wi-Fi firmware didn't load (read back differs)");
    }

    // The NVRAM settings, at the top of RAM.
    fw_start("txt");
    uint32_t size = f_size(&fw_file);
    if (size > 16384)
        fail("the Wi-Fi .txt file is too big");
    static uint8_t cfg[16384 + 8], cfg2[16384 + 8];
    int n = fw_read(cfg, (int)size);
    fw_close();
    n = condense(cfg, n);
    off = chip.socramsize - (uint32_t)n - 4;
    sbmem(true, cfg, n, chip.rambase + off);
    sbmem(false, cfg2, n, chip.rambase + off);
    if (memcmp(cfg, cfg2, (size_t)n))
        fail("the Wi-Fi settings didn't load");
    uint32_t words = (uint32_t)n / 4;
    put4(buf, (words & 0xFFFF) | (~words << 16));
    sbmem(true, buf, 4, chip.rambase + chip.socramsize - 4);
    sbreset(chip.armctl, 0, 0);  // the chip's ARM runs the firmware
}

static void sbenable(void) {
    cfgw(Clkcsr, 0);
    timer_delay_us(1000);
    cfgw(Clkcsr, ReqHT);
    wait_clock(HTavail, 5000);
    cfgw(Clkcsr, cfgr(Clkcsr) | ForceHT);
    timer_delay_us(10000);
    sbwindow(chip.sdregs);
    cfgwritel(chip.sdregs + Sbmboxdata, 4 << 16);  // the protocol version
    cfgwritel(chip.sdregs + Intmask, FrameInt | MailboxInt | Fcchange);
    setbits(Fn0, Ioenable, 1 << Fn2);
    for (int i = 0; !(rd(Fn0, Ioready) & 1 << Fn2); i++) {
        if (i == 10)
            fail("the Wi-Fi firmware didn't start");
        timer_delay_us(100000);
    }
    wrb(Fn0, Intenable, (1 << Fn1) | (1 << Fn2) | 1);
}

// ---- Frames ------------------------------------------------------------------------

static void event(const uint8_t *p, int len);

/// Reads a frame into rxbuf; its length, or 0 if there was none.
static int readpkt(void) {
    packetrw(false, rxbuf, SDPCM);
    int len = (int)get2(rxbuf);
    if (len == 0)
        return 0;
    int lenck = (int)get2(rxbuf + 2);
    if (lenck != (len ^ 0xFFFF) || len < SDPCM || len > PKT) {
        // Out of step: drop the frame.
        cfgw(Framectl, Rfhalt);
        for (int i = 0; i < 1000 && (cfgr(Rfrmcnt + 1) || cfgr(Rfrmcnt)); i++)
            ;
        return 0;
    }
    if (len > SDPCM)
        packetrw(false, rxbuf + SDPCM, len - SDPCM);
    chip.txwindow = rxbuf[9];
    chip.fcmask = rxbuf[8];
    return len;
}

/// Handles the frame in rxbuf.
static void dispatch(int len) {
    int doffset = rxbuf[7];
    switch (rxbuf[5] & 0xF) {
    case 0:  // a command's response
        if (len >= SDPCM + CDC && get2(rxbuf + doffset + 10) == chip.reqid) {
            rsp = rxbuf;
            rsp_len = len;
            rsp_ready = true;
        }
        break;
    case 1:  // an event
        if (len > doffset + 4) {
            int bdc = 4 + (rxbuf[doffset + 3] << 2);
            if (len > doffset + bdc)
                event(rxbuf + doffset + bdc, len - doffset - bdc);
        }
        break;
    case 2:  // data: an Ethernet frame
        if (len > doffset + 4) {
            int bdc = 4 + (rxbuf[doffset + 3] << 2);
            if (len >= doffset + bdc + 14 && rx_handler)
                rx_handler(rxbuf + doffset + bdc, len - doffset - bdc);
        }
        break;
    default:
        break;
    }
}

/// Acknowledges the chip's interrupts; whether a frame is waiting.
static bool service(void) {
    if (!rd(Fn0, Intpend))
        return false;
    sbwindow(chip.sdregs);
    uint32_t ints = cfgreadl(chip.sdregs + Intstatus);
    cfgwritel(chip.sdregs + Intstatus, ints);
    if (ints & MailboxInt) {
        cfgreadl(chip.sdregs + Hostmboxdata);
        cfgwritel(chip.sdregs + Sbmbox, 2);  // ack
    }
    return ints & FrameInt;
}

/// Reads and handles frames for up to `us` microseconds, or until `until` is set.
static void poll(uint32_t us, bool *until) {
    uint64_t end = timer_us() + us;
    bool frames = true;  // look once without waiting for the interrupt
    while (!(until && *until)) {
        if (frames) {
            int len = readpkt();
            if (len) {
                dispatch(len);
                continue;
            }
            frames = false;
        }
        if (timer_us() >= end)
            break;
        frames = service();
        if (!frames)
            timer_delay_us(200);
    }
}

/// Sends a command and waits for its answer; the firmware's status (0: done).
static int wlcmd(bool write, uint32_t op, const void *data, int dlen, void *res, int rlen) {
    int tlen = write ? dlen + rlen : (dlen > rlen ? dlen : rlen);
    int len = SDPCM + CDC + tlen;
    if (len > PKT)
        fail("Wi-Fi command too long");
    memset(txbuf, 0, (size_t)len + 4);
    put2(txbuf, (uint32_t)len);
    put2(txbuf + 2, (uint32_t)~len);
    txbuf[4] = chip.txseq;
    txbuf[7] = SDPCM;  // the data's offset
    uint8_t *q = txbuf + SDPCM;
    put4(q, op);
    put4(q + 4, (uint32_t)tlen);
    put2(q + 8, write ? 2 : 0);
    put2(q + 10, ++chip.reqid);
    if (dlen > 0)
        memcpy(q + CDC, data, (size_t)dlen);
    if (write && rlen > 0)
        memcpy(q + CDC + dlen, res, (size_t)rlen);
    packetrw(true, txbuf, len);
    chip.txseq++;
    rsp = NULL;
    rsp_ready = false;
    uint64_t end = timer_us() + 2000000;
    while (!rsp_ready && timer_us() < end)
        poll(20000, &rsp_ready);
    if (!rsp)
        fail("the Wi-Fi chip didn't answer a command");
    uint8_t *r = rsp + rsp[7];
    rsp = NULL;
    int status = (int)get4(r + 12);
    if (!write && !status && res) {
        int have = rsp_len - (int)(r + CDC - rxbuf);
        memcpy(res, r + CDC, (size_t)(rlen < have ? rlen : have > 0 ? have : 0));
    }
    return status;
}

static void must(int status) {
    if (status)
        fail("the Wi-Fi firmware refused a command");
}

static int setvar(const char *name, const void *val, int len) {
    char buf[PKT - SDPCM - CDC];
    int n = (int)strlen(name) + 1;
    if (n + len > (int)sizeof buf)
        fail("Wi-Fi setting too long");
    memcpy(buf, name, (size_t)n);
    return wlcmd(true, SetVar, buf, n, (void *)val, len);
}

static int setint(const char *name, uint32_t v) {
    uint8_t b[4];
    put4(b, v);
    return setvar(name, b, 4);
}

static int getvar(const char *name, void *val, int len) {
    return wlcmd(false, GetVar, name, (int)strlen(name) + 1, val, len);
}

static int cmdint(uint32_t op, uint32_t v) {
    uint8_t b[4];
    put4(b, v);
    return wlcmd(true, op, b, 4, NULL, 0);
}

/// The regulatory data (.clm_blob), in pieces.
static void clmload(void) {
    enum { Hdr = 12, Size = 1400, Flagclm = 1 << 12, First = 1 << 1, Last = 1 << 2 };
    static uint8_t buf[Hdr + Size + 8];
    fw_start("clm_blob");
    uint32_t left = f_size(&fw_file);
    int flag = Flagclm | First;
    while (!(flag & Last)) {
        int n = fw_read(buf + Hdr, left > Size ? Size : (int)left);
        if (n <= 0)
            break;
        left -= (uint32_t)n;
        if (!left) {
            while (n & 7)
                buf[Hdr + n++] = 0;
            flag |= Last;
        }
        memset(buf, 0, Hdr);
        put2(buf, (uint32_t)flag);
        put2(buf + 2, 2);  // the type: CLM
        put4(buf + 4, (uint32_t)n);
        if (setvar("clmload", buf, Hdr + n)) {
            fw_close();
            fail("the Wi-Fi chip refused its regulatory data");
        }
        flag &= ~First;
    }
    fw_close();
}

static void setcountry(const char *cc) {
    if (!cc || strlen(cc) != 2)
        cc = "XX";
    uint8_t p[12] = {0};
    p[0] = (uint8_t)cc[0], p[1] = (uint8_t)cc[1];
    put4(p + 4, 0xFFFFFFFFu);  // the revision: any
    p[8] = (uint8_t)cc[0], p[9] = (uint8_t)cc[1];
    setvar("country", p, sizeof p);  // a country it doesn't know is not fatal
}

static void wlinit(const char *country) {
    must(getvar("cur_etheraddr", chip.mac, 6));
    setint("assoc_listen", 10);
    cmdint(0x56, 0);  // power saving off
    setint("bus:txglom", 0);
    setint("bcn_timeout", 10);
    setint("assoc_retry_max", 3);
    uint8_t mask[16];
    memset(mask, 0xFF, sizeof mask);
    static const int off[] = {40, 44, 54, 71, 20, 124};  // radio, probe req, if, probe resp, tx fail, ?
    for (unsigned i = 0; i < sizeof off / sizeof off[0]; i++)
        mask[off[i] / 8] &= (uint8_t)~(1 << (off[i] % 8));
    must(setvar("event_msgs", mask, sizeof mask));
    cmdint(0xb9, 0x28);   // SET_SCAN_CHANNEL_TIME
    cmdint(0xbb, 0x28);   // SET_SCAN_UNASSOC_TIME
    cmdint(0x102, 0x82);  // SET_SCAN_PASSIVE_TIME
    must(cmdint(2, 0));   // UP
    memset(chip.version, 0, sizeof chip.version);
    getvar("ver", chip.version, sizeof chip.version - 1);
    char *nl = strchr(chip.version, '\n');
    if (nl)
        *nl = 0;
    setint("roam_off", 1);
    cmdint(0x14, 1);  // SET_INFRA
    cmdint(10, 0);    // SET_PROMISC off
    setcountry(country);
    must(cmdint(2, 1));  // UP
}

// ---- Events and scanning -----------------------------------------------------------

static void scan_result(const uint8_t *p, int len) {
    // escan_result: buflen[4] version[4] sync_id[2] bss_count[2], then bss_info
    if (len < 12 || !scan_found)
        return;
    int nbss = (int)get2(p + 10);
    p += 12, len -= 12;
    if (!nbss || len < 0x7C)
        return;
    wlan_network_t net = {0};
    memcpy(net.bssid, p + 8, 6);
    int n = p[18] < 32 ? p[18] : 32;
    memcpy(net.ssid, p + 19, (size_t)n);
    net.ssid[n] = 0;
    net.channel = p[72];
    net.rssi = (int16_t)get2(p + 78);
    net.security = get2(p + 16) & 0x10 ? 1 : 0;
    uint32_t ieoff = get4(p + 0x74), ielen = get4(p + 0x78);
    if (ielen && ieoff + ielen <= (uint32_t)len) {
        const uint8_t *t = p + ieoff, *et = t + ielen;
        static const uint8_t wpa_oui[4] = {0x00, 0x50, 0xF2, 0x01};
        for (; t + 2 <= et && t + 2 + t[1] <= et; t += 2 + t[1]) {
            if (t[0] == 0x30)
                net.security = 3;
            else if (t[0] == 0xDD && t[1] > 4 && !memcmp(t + 2, wpa_oui, 4) && net.security < 2)
                net.security = 2;
        }
    }
    scan_found(&net);
}

/// An event from the firmware (after the BDC header).
static void event(const uint8_t *p, int len) {
    // An Ethernet header (14), Broadcom's (10), then the event (48), then its data.
    if (len < 14 + 10 + 48)
        return;
    p += 24, len -= 24;
    uint32_t flags = getbe2(p + 2);
    uint32_t type = getbe2(p + 6);
    uint32_t status = getbe4(p + 8);
    uint32_t reason = getbe4(p + 12);
    if (join.active) {
        switch (type) {
        case 0:  // E_SET_SSID
            if (status == 0)
                join.bits |= J_ASSOC;
            else
                join.failed = status == 3 ? WLAN_NONET : WLAN_FAIL;
            break;
        case 16:  // E_LINK
            if (flags & 1)
                join.bits |= J_LINK;
            else
                join.bits = 0;
            break;
        case 46:  // E_PSK_SUP
            if (status == 6)  // keyed
                join.bits |= J_KEYED;
            else if (!((status == 4 || status == 8 || status == 11) && reason == 15))
                join.failed = WLAN_BADAUTH;  // (reason 15 is a time-out at the edge of the cell)
            break;
        case 5: case 6: case 11: case 12:  // E_DEAUTH(_IND), E_DISASSOC(_IND)
            join.bits = 0;
            break;
        }
    }
    if (type == 69) {  // E_ESCAN_RESULT: 8 partial, otherwise the last
        if (status == 8)
            scan_result(p + 48, len - 48);
        else
            scan_done = true;
    }
}

// ---- The interface -----------------------------------------------------------------

/// Starts one of the calls below: false if the chip is off or in use.
#define ENTER(fail_value)                 \
    do {                                  \
        if (!chip.up || busy)             \
            return fail_value;            \
        busy = true;                      \
        if (setjmp(fail_jump)) {          \
            busy = false;                 \
            return fail_value;            \
        }                                 \
    } while (0)

const char *wlan_start(const char *country) {
    if (chip.up)
        return NULL;
    if (busy)
        return "Wi-Fi is busy";
    busy = true;
    if (setjmp(fail_jump)) {
        fw_close();
        chip.up = false;
        busy = false;
        return fail_why;
    }
    memset(&join, 0, sizeof join);
    int id = chip.chipid;
    memset(&chip, 0, sizeof chip);
    chip.chipid = id;
    sdioinit();
    sbinit();
    fwload();
    sbenable();
    poll(100000, NULL);  // the "firmware ready" mailbox
    clmload();
    wlinit(country);
    chip.up = true;
    busy = false;
    return NULL;
}

void wlan_stop(void) {
    ENTER();
    memset(&join, 0, sizeof join);
    cmdint(52, 0);  // DISASSOC
    cmdint(3, 0);   // DOWN
    wrb(Fn0, Ioabort, 1 << 3);  // reset the SDIO side
    chip.up = false;
    busy = false;
}

bool wlan_up(void) {
    return chip.up;
}

const uint8_t *wlan_mac(void) {
    return chip.mac;
}

const char *wlan_version(void) {
    return chip.version;
}

int wlan_chip(int *rev) {
    if (rev)
        *rev = chip.chiprev;
    return chip.chipid;
}

const char *wlan_scan(wlan_found_t *found, bool (*stop)(void)) {
    if (!chip.up)
        return "Wi-Fi is off";
    if (busy)
        return "Wi-Fi is busy";
    busy = true;
    if (setjmp(fail_jump)) {
        scan_found = NULL;
        busy = false;
        return fail_why;
    }
    // escan's parameters: version[4] action[2] sync_id[2] ssidlen[4] ssid[32]
    // bssid[6] bss_type[1] scan_type[1] nprobes[4] active_time[4]
    // passive_time[4] home_time[4] nchans[2] nssids[2] chans[14][2] ssids[1][36]
    static const uint8_t chanspecs[14 * 2] = {
        0x01, 0x2b, 0x02, 0x2b, 0x03, 0x2b, 0x04, 0x2b, 0x05, 0x2e, 0x06, 0x2e, 0x07, 0x2e,
        0x08, 0x2b, 0x09, 0x2b, 0x0a, 0x2b, 0x0b, 0x2b, 0x0c, 0x2b, 0x0d, 0x2b, 0x0e, 0x2b,
    };
    uint8_t params[4 + 2 + 2 + 4 + 32 + 6 + 1 + 1 + 4 * 4 + 2 + 2 + sizeof chanspecs + 36];
    memset(params, 0, sizeof params);
    uint8_t *p = params;
    put4(p, 1), p += 4;       // version
    put2(p, 1), p += 2;       // action: start
    put2(p, 0x1234), p += 2;  // sync id
    p += 4 + 32;              // no SSID: all of them
    memset(p, 0xFF, 6), p += 6;
    *p++ = 2;  // any kind of network
    *p++ = 0;  // active: send probe requests
    for (int i = 0; i < 4; i++)
        put4(p, 0xFFFFFFFFu), p += 4;  // the firmware's times
    put2(p, 14), p += 2;
    put2(p, 1), p += 2;
    memcpy(p, chanspecs, sizeof chanspecs), p += sizeof chanspecs;
    p += 36;  // the wildcard SSID
    scan_found = found;
    scan_done = false;
    cmdint(49, 0);  // passive scanning off
    if (setvar("escan", params, (int)(p - params))) {
        scan_found = NULL;
        busy = false;
        return "the Wi-Fi chip wouldn't scan";
    }
    uint64_t end = timer_us() + 15000000;
    while (!scan_done && timer_us() < end) {
        poll(50000, &scan_done);
        if (stop && stop()) {
            scan_found = NULL;
            busy = false;
            return "stopped";
        }
    }
    scan_found = NULL;
    busy = false;
    return scan_done ? NULL : "the scan didn't finish";
}

void wlan_set_rx(wlan_rx_t *rx) {
    rx_handler = rx;
}

void wlan_poll(void) {
    ENTER();
    if (service())
        poll(0, NULL);
    busy = false;
}

bool wlan_send(const uint8_t *frame, int len) {
    if (len <= 0 || len > PKT - SDPCM - 4)
        return false;
    ENTER(false);
    // Wait (a little) for the firmware to have room: it says how many frames
    // it will take in each frame it sends.
    uint64_t end = timer_us() + 100000;
    for (;;) {
        uint8_t credit = (uint8_t)(chip.txwindow - chip.txseq);
        if (credit && credit <= 0x40 && !(chip.fcmask & 1 << 2))
            break;
        if (timer_us() > end) {
            busy = false;
            return false;
        }
        poll(1000, NULL);
    }
    int total = SDPCM + 4 + len;
    memset(txbuf, 0, SDPCM + 4);
    put2(txbuf, (uint32_t)total);
    put2(txbuf + 2, (uint32_t)~total);
    txbuf[4] = chip.txseq;
    txbuf[5] = 2;  // data
    txbuf[7] = SDPCM;
    txbuf[SDPCM] = 0x20;  // the BDC header: version 2
    memcpy(txbuf + SDPCM + 4, frame, (size_t)len);
    packetrw(true, txbuf, total);
    chip.txseq++;
    busy = false;
    return true;
}

const char *wlan_join(const char *ssid, const char *password) {
    if (!chip.up)
        return "Wi-Fi is off";
    int n = (int)strlen(ssid), k = password ? (int)strlen(password) : 0;
    if (n < 1 || n > 32)
        return "the network's name must be 1 to 32 characters";
    if (k && (k < 8 || k > 64))
        return "a WPA2 password is 8 to 63 characters (or 64 hex digits)";
    if (busy)
        return "Wi-Fi is busy";
    busy = true;
    if (setjmp(fail_jump)) {
        busy = false;
        join.active = false;
        return fail_why;
    }
    memset(&join, 0, sizeof join);
    cmdint(52, 0);  // DISASSOC from any network before
    uint8_t b[68];
    if (k) {
        setint("wsec", 4);  // AES
        put4(b, 0), put4(b + 4, 1);
        setvar("bsscfg:sup_wpa", b, 8);  // the firmware's own supplicant does the handshake
        put4(b + 4, 0xFFFFFFFFu);
        setvar("bsscfg:sup_wpa2_eapver", b, 8);
        put4(b + 4, 2500);
        setvar("bsscfg:sup_wpa_tmo", b, 8);
        timer_delay_us(2000);
        memset(b, 0, sizeof b);
        put2(b, (uint32_t)k);
        put2(b + 2, k < 64 ? 1 : 0);  // a passphrase, or the key in hex
        memcpy(b + 4, password, (size_t)k);
        must(wlcmd(true, 268, b, 68, NULL, 0));  // SET_WSEC_PMK
        must(cmdint(20, 1));     // SET_INFRA
        must(cmdint(22, 0));     // SET_AUTH: open system
        must(cmdint(165, 0x80)); // SET_WPA_AUTH: WPA2 PSK
    } else {
        setint("wsec", 0);
        put4(b, 0), put4(b + 4, 0);
        setvar("bsscfg:sup_wpa", b, 8);
        must(cmdint(20, 1));
        must(cmdint(22, 0));
        must(cmdint(165, 0));
    }
    join.secure = k > 0;
    join.active = true;
    memset(b, 0, 36);
    put4(b, (uint32_t)n);
    memcpy(b + 4, ssid, (size_t)n);
    if (wlcmd(true, 26, b, 36, NULL, 0)) {  // SET_SSID: starts the join
        join.active = false;
        busy = false;
        return "the Wi-Fi chip wouldn't join";
    }
    busy = false;
    return NULL;
}

void wlan_leave(void) {
    ENTER();
    memset(&join, 0, sizeof join);
    cmdint(52, 0);  // DISASSOC
    busy = false;
}

int wlan_link(void) {
    if (!chip.up || !join.active)
        return WLAN_DOWN;
    if (join.failed)
        return join.failed;
    uint8_t need = J_ASSOC | J_LINK | (join.secure ? J_KEYED : 0);
    return (join.bits & need) == need ? WLAN_JOINED : WLAN_JOINING;
}

int wlan_rssi(void) {
    ENTER(0);
    uint8_t b[4] = {0};
    int r = wlcmd(false, 127, NULL, 0, b, 4) ? 0 : (int)(int32_t)get4(b);  // GET_RSSI
    busy = false;
    return r;
}
