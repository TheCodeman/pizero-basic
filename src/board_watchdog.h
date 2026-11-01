/**
 * @file board_watchdog.h
 * @brief The hardware watchdog BASIC's WATCHDOG arms (basic_port_serial.c).
 * Long system work (disk reads and writes, XMODEM, Wi-Fi waits, the
 * full-screen editor) feeds it, so only a program that stops feeding it, or a
 * real hang, resets the board.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>

/// Reloads the watchdog if it is armed (cheap: a register write).
void board_watchdog_feed(void);
