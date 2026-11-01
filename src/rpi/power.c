/**
 * @file power.c
 * @brief Reset through the PM block's watchdog.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rpi.h"

#define PM_RSTC (PM_BASE + 0x1C)
#define PM_WDOG (PM_BASE + 0x24)
#define PM_PASSWORD 0x5A000000u

void board_reboot(void) {
    REG(PM_WDOG) = PM_PASSWORD | 10;  // ticks of about 16 us
    REG(PM_RSTC) = PM_PASSWORD | (REG(PM_RSTC) & ~0x30u) | 0x20u;  // full reset
    for (;;)
        __asm volatile("wfi");
}
