/**
 * @file emmc.h
 * @brief The SD card slot (emmc.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// Finds and sets up the card: NULL, or what went wrong ("none": no card).
const char *emmc_init(void);
/// The card was set up.
bool emmc_ready(void);
/// Its size in 512-byte sectors.
uint32_t emmc_sectors(void);
/// Reads or writes `count` 512-byte sectors; false on an error.
bool emmc_read(uint8_t *buf, uint32_t sector, uint32_t count);
bool emmc_write(const uint8_t *buf, uint32_t sector, uint32_t count);
