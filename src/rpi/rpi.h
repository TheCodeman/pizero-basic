/**
 * @file rpi.h
 * @brief The BCM2835 (Raspberry Pi Zero / Zero W) as this port uses it:
 * peripheral addresses and the small drivers in src/rpi/.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// The peripherals, as the ARM sees them on the BCM2835.
#define PERI_BASE 0x20000000u

#define SYSTIMER_BASE (PERI_BASE + 0x003000)
#define IRQ_BASE (PERI_BASE + 0x00B200)
#define MBOX_BASE (PERI_BASE + 0x00B880)
#define PM_BASE (PERI_BASE + 0x100000)
#define GPIO_BASE (PERI_BASE + 0x200000)
#define UART0_BASE (PERI_BASE + 0x201000)

/// A 32-bit peripheral register.
#define REG(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

/// The ARM's view of a bus address the GPU hands back (it uses 0xC0000000+
/// aliases for uncached RAM).
#define BUS_TO_ARM(a) ((uint32_t)(a) & 0x3FFFFFFFu)

/// Memory barrier: needed between accesses to different peripherals.
static inline void dmb(void) {
    __asm volatile("mcr p15, 0, %0, c7, c10, 5" ::"r"(0) : "memory");
}
/// Drain the write buffer (DSB).
static inline void dsb(void) {
    __asm volatile("mcr p15, 0, %0, c7, c10, 4" ::"r"(0) : "memory");
}

// ---- system timer (timer.c) ----------------------------------------------------

/// Microseconds since power-up (the free-running 1 MHz system timer).
uint64_t timer_us(void);
/// Busy-waits `us` microseconds.
void timer_delay_us(uint32_t us);

// ---- GPIO (gpio.c) -------------------------------------------------------------

/// GPIO function select values.
enum { GPIO_IN = 0, GPIO_OUT = 1, GPIO_ALT0 = 4, GPIO_ALT1 = 5, GPIO_ALT2 = 6,
       GPIO_ALT3 = 7, GPIO_ALT4 = 3, GPIO_ALT5 = 2 };
/// Pull: none, down or up.
enum { GPIO_PULL_NONE = 0, GPIO_PULL_DOWN = 1, GPIO_PULL_UP = 2 };
void gpio_function(int pin, int func);
void gpio_pull(int pin, int pull);
void gpio_write(int pin, int level);
int gpio_read(int pin);

// ---- serial console, PL011 on GPIO14 (TX) and GPIO15 (RX) (uart.c) --------------

/// Sets up UART0 at `baud`, 8N1, on GPIO14/15.
void uart_init(unsigned baud);
void uart_putc(char c);
/// The next byte received, or -1 if none is waiting.
int uart_getc(void);
/// Receives by interrupt from now on (into a 4 KB ring). Needs irq_init.
void uart_enable_irq(void);
/// UART0's interrupt (main connects it).
void uart_irq(void *unused);
/// COM1: the PL011 lent to BASIC. While open, the console's uart_putc and
/// uart_getc do nothing; close puts the console back (115200 8N1).
void uart_com_open(unsigned baud, int data_bits, char parity, int stop_bits);
void uart_com_close(void);
/// RTS/CTS on GPIO17/16.
void uart_com_flow(bool on);
void uart_com_break(bool on);
void uart_com_write(const char *data, int n);
int uart_com_getc(void);
int uart_com_waiting(void);
int uart_com_room(void);

// ---- interrupts (irq.c) -----------------------------------------------------------------

/// Turns IRQs on at the ARM.
void irq_init(void);
/// Runs fn(param) for interrupt `irq` (0-63) and enables it.
void irq_connect(unsigned irq, void (*fn)(void *), void *param);

// ---- mailbox to the VideoCore firmware (mailbox.c) --------------------------------

/// Sends a property-tag buffer (16-byte aligned, in uncached-safe memory) on
/// channel 8 and waits for the answer. False if the firmware refused it.
bool mbox_property(volatile uint32_t *buf);
/// The ARM's memory: base and size in bytes. False if the firmware didn't say.
bool mbox_arm_memory(uint32_t *base, uint32_t *size);
/// The board's revision code (0x9000c1 is a Zero W), or 0.
uint32_t mbox_board_revision(void);
/// A clock's rate in Hz (2 UART, 3 ARM, 4 core), or 0.
uint32_t mbox_clock_rate(uint32_t clock_id);
/// The SoC temperature in thousandths of a degree C, or 0.
uint32_t mbox_temperature(void);

// ---- MMU and caches (mmu.c) ---------------------------------------------------------

/// Maps memory flat (1 MB sections): RAM cached, the peripherals as device
/// memory. Turns on the MMU, the caches and branch prediction.
void mmu_init(uint32_t ram_top);
/// Maps [virt, virt+len) to [phys, ...) in 4 KB write-through pages (all
/// within one MB): the framebuffer. False if it can't.
bool mmu_map_write_through(uint32_t virt, uint32_t phys, uint32_t len);
/// Cleans and invalidates the data cache for [addr, addr+len): needed before
/// the GPU or a DMA engine reads memory the ARM wrote, and after it writes.
void dcache_clean_invalidate(const void *addr, size_t len);

// ---- reset (power.c) ----------------------------------------------------------------

/// Restarts the board through the PM watchdog.
void board_reboot(void) __attribute__((noreturn));

// ---- PWM, I2C and SPI0 (pins.c) -----------------------------------------------------

/// The PWM channel on a pin (0 on GPIO12 and 18, 1 on 13 and 19), or -1.
int pwm_channel(int pin);
/// PWM on `pin` at `freq` Hz (0: off) and duty 0-100%; the pin's channel is
/// shared with its partner. Returns the frequency it runs at.
double pwm_set(int pin, double freq, double duty);
/// Opens I2C bus 0 (GPIO0/1) or 1 (GPIO2/3) at `khz`.
bool i2c_init(int bus, unsigned khz);
void i2c_deinit(void);
/// Writes n bytes to device addr: n, -1 if no answer, -2 if not open.
int i2c_write_bytes(int addr, const uint8_t *data, int n);
/// Writes nw bytes (0-16), then with a repeated start reads n: n, -1 or -2.
int i2c_read_bytes(int addr, const uint8_t *wr, int nw, uint8_t *data, int n);
/// Sets SPI0 to `khz` (rounded down) and mode 0-3; returns the kHz it runs at.
unsigned spi_init(unsigned khz, int mode);
/// Sends n bytes from out (NULL: zeros) and keeps those received in in (may
/// be NULL). False if it timed out.
bool spi_xfer(const uint8_t *out, uint8_t *in, int n);
