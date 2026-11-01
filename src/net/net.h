/**
 * @file net.h
 * @brief The network over the Wi-Fi chip (net.c): joining, lwIP, DHCP and the
 * clock by SNTP. Polled: net_poll runs it, from the key checks BASIC makes.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdbool.h>

/// The link's state, as the Pico W's CYW43_LINK_* (WIFI(3) gives it).
enum { NET_DOWN = 0, NET_JOIN = 1, NET_NOIP = 2, NET_UP = 3, NET_FAIL = -1, NET_NONET = -2, NET_BADAUTH = -3 };

/// Starts the chip (if it is off) and the network stack.
/// @return NULL, or why not
const char *net_start(void);

/// Starts joining `ssid` (and keeps at it if the link drops).
/// @return NULL, or why it couldn't start
const char *net_connect(const char *ssid, const char *password);

/// Leaves the network and turns the radio off.
void net_stop(void);

/// Runs the network: frames in, timers. Cheap to call often.
void net_poll(void);

/// The link (NET_*).
int net_link(void);

/// Words for a NET_* value.
const char *net_link_text(int link);

/// The address ("" before DHCP gives one).
const char *net_ip(void);

/// The network joined or being joined ("" if none).
const char *net_ssid(void);

/// The signal, dBm (0 if not joined).
int net_rssi(void);

/// SNTP has set the clock.
bool net_clock_set(void);

/// Asks the NTP server for the time now and waits for it (Esc stops).
/// @return NULL, or why not
const char *net_ntp(const char *server, bool (*stop)(void));
