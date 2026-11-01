/**
 * @file sdio.h
 * @brief SDIO to the Wi-Fi chip on the EMMC controller (sdio.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// Routes EMMC to GPIO34-39, resets it, and starts the clock at 400 kHz.
bool sdio_init(void);
/// The bus clock, at most `hz`.
void sdio_clock(uint32_t hz);
/// The host's side of the bus width (the card's is set with CMD52).
void sdio_bus_width4(bool on);
/// Sends a command (0, 3, 5, 7, 52, 53); resp gets its response. False on an error.
bool sdio_cmd(unsigned cmd, uint32_t arg, uint32_t *resp);
/// The block size and count for the next CMD53.
void sdio_setup(unsigned block, unsigned count);
/// Moves CMD53's data: `count` blocks of `block` bytes. False on an error.
bool sdio_io(bool write, uint8_t *buf, unsigned block, unsigned count);
/// The card has raised its interrupt (and it is cleared).
bool sdio_card_irq(void);
