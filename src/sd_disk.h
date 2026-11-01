/**
 * @file sd_disk.h
 * @brief The SD card as FatFs's drive 0, "SD:" (sd_disk.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdint.h>

/// Sets up the card and mounts it: NULL, or why not ("none": no card).
const char *sd_disk_mount(void);
/// Its size and free space in KB (0 if it isn't mounted).
void sd_disk_space(uint32_t *total_kb, uint32_t *free_kb);
