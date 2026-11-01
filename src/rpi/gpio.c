/**
 * @file gpio.c
 * @brief BCM2835 GPIO: function select, pulls, levels.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rpi.h"

#define GPFSEL0 (GPIO_BASE + 0x00)
#define GPSET0 (GPIO_BASE + 0x1C)
#define GPCLR0 (GPIO_BASE + 0x28)
#define GPLEV0 (GPIO_BASE + 0x34)
#define GPPUD (GPIO_BASE + 0x94)
#define GPPUDCLK0 (GPIO_BASE + 0x98)

void gpio_function(int pin, int func) {
    uint32_t reg = GPFSEL0 + 4u * (uint32_t)(pin / 10);
    int shift = (pin % 10) * 3;
    uint32_t v = REG(reg);
    v &= ~(7u << shift);
    v |= (uint32_t)(func & 7) << shift;
    REG(reg) = v;
}

/// The BCM2835's pull sequence: set the control, wait 150 cycles, clock it
/// into the pin, wait, take both away.
void gpio_pull(int pin, int pull) {
    uint32_t clk = GPPUDCLK0 + 4u * (uint32_t)(pin / 32);
    REG(GPPUD) = (uint32_t)pull;
    timer_delay_us(2);
    REG(clk) = 1u << (pin % 32);
    timer_delay_us(2);
    REG(GPPUD) = 0;
    REG(clk) = 0;
}

void gpio_write(int pin, int level) {
    uint32_t bit = 1u << (pin % 32);
    REG((level ? GPSET0 : GPCLR0) + 4u * (uint32_t)(pin / 32)) = bit;
}

int gpio_read(int pin) {
    return (REG(GPLEV0 + 4u * (uint32_t)(pin / 32)) >> (pin % 32)) & 1;
}
