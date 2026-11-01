/**
 * @file usb_input.h
 * @brief The USB keyboard and mouse (usb_input.c, through USPi).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdbool.h>

/// Starts USB and finds the keyboard and mouse: NULL, or why not.
const char *usb_input_init(void);
bool usb_keyboard_present(void);
bool usb_mouse_present(void);
/// From the main loop now and then: sends the Caps Lock LED.
void usb_input_poll(void);
/// KEYDOWN: the key with PC scan code `scan` is held (0: a keyboard is there).
bool usb_key_down(int scan);
