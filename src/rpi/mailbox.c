/**
 * @file mailbox.c
 * @brief The property mailbox (channel 8) to the VideoCore firmware.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rpi.h"

#define MBOX_READ (MBOX_BASE + 0x00)
#define MBOX_STATUS (MBOX_BASE + 0x18)
#define MBOX_WRITE (MBOX_BASE + 0x20)
#define MBOX_FULL 0x80000000u
#define MBOX_EMPTY 0x40000000u
#define MBOX_CH_PROP 8
/// The GPU sees the ARM's RAM through its L2 cache at this bus alias (BCM2835).
#define BUS_L2_ALIAS 0x40000000u

bool mbox_property(volatile uint32_t *buf) {
    uint32_t addr = (uint32_t)(uintptr_t)buf;
    uint32_t msg = (addr | BUS_L2_ALIAS) | MBOX_CH_PROP;
    dcache_clean_invalidate((const void *)buf, buf[0]);
    dmb();
    while (REG(MBOX_STATUS) & MBOX_FULL)
        ;
    REG(MBOX_WRITE) = msg;
    for (;;) {
        while (REG(MBOX_STATUS) & MBOX_EMPTY)
            ;
        if (REG(MBOX_READ) == msg)
            break;
    }
    dmb();
    dcache_clean_invalidate((const void *)buf, buf[0]);
    return buf[1] == 0x80000000u;
}

/// One tag with up to 2 words in and 2 out; returns the tag's value words.
static bool one_tag(uint32_t tag, uint32_t in0, uint32_t in1, int nin, uint32_t out[2]) {
    static volatile uint32_t buf[8] __attribute__((aligned(16)));
    buf[0] = sizeof buf;
    buf[1] = 0;
    buf[2] = tag;
    buf[3] = 8;  // value buffer size
    buf[4] = (uint32_t)nin * 4;
    buf[5] = in0;
    buf[6] = in1;
    buf[7] = 0;  // end tag
    if (!mbox_property(buf) || !(buf[4] & 0x80000000u))
        return false;
    out[0] = buf[5];
    out[1] = buf[6];
    return true;
}

bool mbox_arm_memory(uint32_t *base, uint32_t *size) {
    uint32_t v[2];
    if (!one_tag(0x00010005, 0, 0, 0, v))
        return false;
    *base = v[0];
    *size = v[1];
    return true;
}

uint32_t mbox_board_revision(void) {
    uint32_t v[2];
    return one_tag(0x00010002, 0, 0, 0, v) ? v[0] : 0;
}

uint32_t mbox_clock_rate(uint32_t clock_id) {
    uint32_t v[2];
    return one_tag(0x00030002, clock_id, 0, 1, v) ? v[1] : 0;
}

uint32_t mbox_temperature(void) {
    uint32_t v[2];
    return one_tag(0x00030006, 0, 0, 1, v) ? v[1] : 0;
}

/// Sets the ARM to the most the firmware allows (1 GHz on a Zero); returns
/// the new rate, or 0.
uint32_t mbox_arm_full_speed(void) {
    uint32_t v[2];
    if (!one_tag(0x00030004, 3, 0, 1, v) || v[1] == 0)  // max ARM clock
        return 0;
    uint32_t max = v[1];
    if (!one_tag(0x00038002, 3, max, 2, v))  // set it
        return 0;
    return v[1];
}
