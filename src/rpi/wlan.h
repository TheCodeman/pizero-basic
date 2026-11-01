/**
 * @file wlan.h
 * @brief The Zero W's Wi-Fi chip (CYW43430) over SDIO (wlan.c): bring-up,
 * firmware, and scanning. Polled: the caller waits while the chip works.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// Where the chip's firmware files are (from Raspberry Pi OS's firmware-nonfree).
#define WLAN_FIRMWARE_DIR "SD:/firmware/"

/// Starts the chip and loads its firmware, once.
/// @param country two letters (XX: anywhere)
/// @return NULL, or why it failed
const char *wlan_start(const char *country);

/// Turns the radio off (wlan_start brings it back).
void wlan_stop(void);

/// The chip is up.
bool wlan_up(void);

/// The chip's MAC address (once up).
const uint8_t *wlan_mac(void);

/// The firmware's version line (once up).
const char *wlan_version(void);

/// The chip's id and revision, e.g. 43430 and 1 (0 before the first start).
int wlan_chip(int *rev);

/// One network a scan found.
typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    int16_t rssi;     ///< the signal, dBm
    uint8_t channel;
    uint8_t security; ///< 0 open, 1 WEP, 2 WPA, 3 WPA2
} wlan_network_t;

/// Called for each access point a scan hears (the same network may come more than once).
typedef void wlan_found_t(const wlan_network_t *net);

/// Scans all 2.4 GHz channels.
/// @param found called for each result
/// @param stop polled while scanning: true stops it
/// @return NULL, or why it failed ("stopped" if stop said so)
const char *wlan_scan(wlan_found_t *found, bool (*stop)(void));

/// Called with each Ethernet frame the chip receives (from inside wlan_poll
/// or another call here: copy it and handle it later).
typedef void wlan_rx_t(const uint8_t *frame, int len);
void wlan_set_rx(wlan_rx_t *rx);

/// Reads what the chip has for us (frames, events). Cheap when there is nothing.
void wlan_poll(void);

/// Sends an Ethernet frame; false if the chip is off, busy or has no room.
bool wlan_send(const uint8_t *frame, int len);

/// Starts joining a network (WPA2 with a password, open without); wlan_link says how it goes.
/// @return NULL, or why it couldn't start
const char *wlan_join(const char *ssid, const char *password);

/// Leaves the network.
void wlan_leave(void);

/// The link: as the Pico W's CYW43_LINK_* values where they match.
enum { WLAN_DOWN = 0, WLAN_JOINING = 1, WLAN_JOINED = 2, WLAN_FAIL = -1, WLAN_NONET = -2, WLAN_BADAUTH = -3 };
int wlan_link(void);

/// The signal from the access point, dBm (0 if not joined).
int wlan_rssi(void);
