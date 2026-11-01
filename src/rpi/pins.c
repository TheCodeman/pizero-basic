/**
 * @file pins.c
 * @brief The BCM2835's PWM, I2C (BSC) and SPI0 controllers, as BASIC's PWM,
 * I2C and SPI commands use them (see rpi.h).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rpi.h"

#define CM_PASSWORD 0x5A000000u
#define CM_PWMCTL (PERI_BASE + 0x1010A0)
#define CM_PWMDIV (PERI_BASE + 0x1010A4)
#define CM_ENAB (1u << 4)
#define CM_BUSY (1u << 7)
#define CM_SRC_OSC 1u  ///< the 19.2 MHz crystal

#define PWM_BASE (PERI_BASE + 0x20C000)
#define PWM_CTL (PWM_BASE + 0x00)
#define PWM_RNG(ch) (PWM_BASE + 0x10 + 0x10 * (ch))
#define PWM_DAT(ch) (PWM_BASE + 0x14 + 0x10 * (ch))
#define PWM_PWEN(ch) (1u << (8 * (ch)))
#define PWM_MSEN(ch) (1u << (8 * (ch) + 7))

/// The PWM clock: the crystal / 2.
#define PWM_CLOCK_HZ 9600000u

// ---- PWM ----------------------------------------------------------------------------

int pwm_channel(int pin) {
    switch (pin) {
    case 12: case 18: return 0;
    case 13: case 19: return 1;
    default: return -1;
    }
}

/// Starts the PWM clock, once.
static void pwm_clock_start(void) {
    static bool started;
    if (started)
        return;
    REG(CM_PWMCTL) = CM_PASSWORD | (REG(CM_PWMCTL) & ~CM_ENAB & 0xFFFFFF);
    uint64_t end = timer_us() + 10000;
    while ((REG(CM_PWMCTL) & CM_BUSY) && timer_us() < end)
        ;
    REG(CM_PWMDIV) = CM_PASSWORD | (19200000u / PWM_CLOCK_HZ) << 12;
    REG(CM_PWMCTL) = CM_PASSWORD | CM_SRC_OSC;
    REG(CM_PWMCTL) = CM_PASSWORD | CM_SRC_OSC | CM_ENAB;
    end = timer_us() + 10000;
    while (!(REG(CM_PWMCTL) & CM_BUSY) && timer_us() < end)
        ;
    started = true;
}

double pwm_set(int pin, double freq, double duty) {
    int ch = pwm_channel(pin);
    if (ch < 0)
        return 0;
    if (freq <= 0) {
        REG(PWM_CTL) &= ~(PWM_PWEN(ch) | PWM_MSEN(ch));
        return 0;
    }
    pwm_clock_start();
    double range = PWM_CLOCK_HZ / freq;
    if (range < 2)
        range = 2;
    if (range > 4294967295.0)
        range = 4294967295.0;
    uint32_t rng = (uint32_t)(range + 0.5);
    if (duty < 0)
        duty = 0;
    if (duty > 100)
        duty = 100;
    REG(PWM_RNG(ch)) = rng;
    REG(PWM_DAT(ch)) = (uint32_t)(rng * duty / 100.0 + 0.5);
    REG(PWM_CTL) |= PWM_PWEN(ch) | PWM_MSEN(ch);  // mark-space: high for DAT of RNG
    gpio_function(pin, pin < 18 ? GPIO_ALT0 : GPIO_ALT5);
    return (double)PWM_CLOCK_HZ / rng;
}

// ---- I2C: the BSC controllers -------------------------------------------------------

#define BSC0_BASE (PERI_BASE + 0x205000)
#define BSC1_BASE (PERI_BASE + 0x804000)
#define BSC_C 0x00
#define BSC_S 0x04
#define BSC_DLEN 0x08
#define BSC_A 0x0C
#define BSC_FIFO 0x10
#define BSC_DIV 0x14
#define BSC_CLKT 0x1C

#define C_READ (1u << 0)
#define C_CLEAR (3u << 4)
#define C_ST (1u << 7)
#define C_I2CEN (1u << 15)
#define S_TA (1u << 0)
#define S_DONE (1u << 1)
#define S_TXD (1u << 4)
#define S_RXD (1u << 5)
#define S_ERR (1u << 8)
#define S_CLKT (1u << 9)

static uint32_t bsc;  ///< the open controller's base, or 0

#define BSC(r) REG(bsc + (r))

bool i2c_init(int bus, unsigned khz) {
    bsc = bus ? BSC1_BASE : BSC0_BASE;
    uint32_t core = mbox_clock_rate(4);
    if (!core)
        core = 250000000;
    uint32_t div = core / (khz * 1000u);
    div = (div + 1) & ~1u;
    if (div < 2)
        div = 2;
    if (div > 0xFFFE)
        div = 0xFFFE;
    BSC(BSC_DIV) = div;
    BSC(BSC_CLKT) = 0x40;  // clock stretching: up to 64 SCL periods
    BSC(BSC_C) = C_I2CEN | C_CLEAR;
    BSC(BSC_S) = S_CLKT | S_ERR | S_DONE;
    return true;
}

void i2c_deinit(void) {
    if (bsc)
        BSC(BSC_C) = 0;
    bsc = 0;
}

/// Waits for the transfer to end, moving bytes; false on a NACK or timeout.
static bool bsc_run(const uint8_t *out, uint8_t *in, int n, unsigned timeout_us) {
    int done = 0;
    uint64_t end = timer_us() + timeout_us;
    for (;;) {
        uint32_t s = BSC(BSC_S);
        if (out)
            while (done < n && (BSC(BSC_S) & S_TXD))
                BSC(BSC_FIFO) = out[done++];
        if (in)
            while (done < n && (BSC(BSC_S) & S_RXD))
                in[done++] = (uint8_t)BSC(BSC_FIFO);
        if (s & (S_ERR | S_CLKT)) {
            BSC(BSC_C) = C_I2CEN | C_CLEAR;
            BSC(BSC_S) = S_CLKT | S_ERR | S_DONE;
            return false;
        }
        if (s & S_DONE) {
            if (in)
                while (done < n && (BSC(BSC_S) & S_RXD))
                    in[done++] = (uint8_t)BSC(BSC_FIFO);
            BSC(BSC_S) = S_DONE;
            return done == n;
        }
        if (timer_us() > end) {
            BSC(BSC_C) = C_I2CEN | C_CLEAR;
            return false;
        }
    }
}

static unsigned i2c_timeout(int n) {
    return 5000u + (unsigned)n * 500u;
}

int i2c_write_bytes(int addr, const uint8_t *data, int n) {
    if (!bsc)
        return -2;
    BSC(BSC_C) = C_I2CEN | C_CLEAR;
    BSC(BSC_S) = S_CLKT | S_ERR | S_DONE;
    BSC(BSC_A) = (uint32_t)addr;
    BSC(BSC_DLEN) = (uint32_t)n;
    // Up to 16 bytes go in the FIFO before the start; bsc_run sends the rest.
    int pre = 0;
    while (pre < n && pre < 16)
        BSC(BSC_FIFO) = data[pre++];
    BSC(BSC_C) = C_I2CEN | C_ST;
    return bsc_run(data + pre, NULL, n - pre, i2c_timeout(n)) ? n : -1;
}

int i2c_read_bytes(int addr, const uint8_t *wr, int nw, uint8_t *data, int n) {
    if (!bsc)
        return -2;
    BSC(BSC_C) = C_I2CEN | C_CLEAR;
    BSC(BSC_S) = S_CLKT | S_ERR | S_DONE;
    BSC(BSC_A) = (uint32_t)addr;
    if (nw > 0) {
        // A write then a repeated start: start the write (at most the FIFO's
        // 16 bytes), and once it is under way, the read. The controller
        // makes a repeated start between them.
        BSC(BSC_DLEN) = (uint32_t)nw;
        for (int i = 0; i < nw; i++)
            BSC(BSC_FIFO) = wr[i];
        BSC(BSC_C) = C_I2CEN | C_ST;
        uint64_t end = timer_us() + 1000;
        while (!(BSC(BSC_S) & (S_TA | S_ERR | S_DONE)) && timer_us() < end)
            ;
        if (BSC(BSC_S) & S_ERR) {
            BSC(BSC_C) = C_I2CEN | C_CLEAR;
            BSC(BSC_S) = S_CLKT | S_ERR | S_DONE;
            return -1;
        }
    }
    BSC(BSC_DLEN) = (uint32_t)n;
    BSC(BSC_C) = C_I2CEN | C_ST | C_READ;
    return bsc_run(NULL, data, n, i2c_timeout(n + nw)) ? n : -1;
}

// ---- SPI0 ---------------------------------------------------------------------------

#define SPI_BASE (PERI_BASE + 0x204000)
#define SPI_CS (SPI_BASE + 0x00)
#define SPI_FIFO (SPI_BASE + 0x04)
#define SPI_CLK (SPI_BASE + 0x08)
#define SPI_CPHA (1u << 2)
#define SPI_CPOL (1u << 3)
#define SPI_CLEAR (3u << 4)
#define SPI_TA (1u << 7)
#define SPI_DONE (1u << 16)
#define SPI_RXD (1u << 17)
#define SPI_TXD (1u << 18)

static uint32_t spi_mode_bits;

unsigned spi_init(unsigned khz, int mode) {
    uint32_t core = mbox_clock_rate(4);
    if (!core)
        core = 250000000;
    uint32_t div = (core + khz * 1000u - 1) / (khz * 1000u);
    div = (div + 1) & ~1u;
    if (div < 2)
        div = 2;
    if (div > 65534)
        div = 65534;
    REG(SPI_CLK) = div;
    spi_mode_bits = (mode & 1 ? SPI_CPHA : 0) | (mode & 2 ? SPI_CPOL : 0);
    REG(SPI_CS) = spi_mode_bits | SPI_CLEAR;
    return core / div / 1000;
}

bool spi_xfer(const uint8_t *out, uint8_t *in, int n) {
    REG(SPI_CS) = spi_mode_bits | SPI_CLEAR | SPI_TA;
    int sent = 0, got = 0;
    // At 1 kHz a byte takes 8 ms: allow 20 ms each, and a second besides.
    uint64_t end = timer_us() + 1000000u + (uint64_t)n * 20000u;
    while (got < n) {
        if (timer_us() > end) {
            REG(SPI_CS) = spi_mode_bits | SPI_CLEAR;
            return false;
        }
        while (sent < n && sent - got < 16 && (REG(SPI_CS) & SPI_TXD))
            REG(SPI_FIFO) = out ? out[sent++] : (sent++, 0);
        while (got < n && (REG(SPI_CS) & SPI_RXD)) {
            uint8_t b = (uint8_t)REG(SPI_FIFO);
            if (in)
                in[got] = b;
            got++;
        }
    }
    while (!(REG(SPI_CS) & SPI_DONE) && timer_us() < end)
        ;
    REG(SPI_CS) = spi_mode_bits;
    return true;
}
