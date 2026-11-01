/**
 * @file config.h
 * @brief FLASH:/CONFIG.TXT: the board's settings, read at power-up.
 *
 * KEY = value lines (any case); # ' or ; start a comment. Without the file
 * the build's defaults hold (the Cytron Maker Pi Pico's pins). Changes take
 * effect at the next power-up or reset.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>

#ifndef CONFIG_PATH
#define CONFIG_PATH "FLASH:/CONFIG.TXT"  ///< where the settings are
#endif
#define CONFIG_PULLUPS 4  ///< extra pins SD_PULLUPS can hold up

/// The settings in force.
typedef struct {
    bool sd;          ///< the SD card is used
    int sd_spi;       ///< its SPI controller, 0 or 1 (from the pins)
    int sd_sck, sd_mosi, sd_miso, sd_cs;  ///< its pins
    int sd_mode;      ///< SPI mode 0-3
    int sd_pullups[CONFIG_PULLUPS];  ///< pins held up while it is used (DAT1, DAT2)
    int sd_npullups;  ///< how many
    int speaker;      ///< BEEP, SOUND and PLAY's pin
    int neopixel;     ///< NEOPIXEL OPEN's default pin
    bool from_file;   ///< CONFIG.TXT was read
    bool sd_bad;      ///< the SD card is off because its pins were wrong
    bool wifi;        ///< WIFI = ON (connect at power-up when there is a network)
    char wifi_ssid[33];      ///< WIFI_SSID: the network
    char wifi_password[64];  ///< WIFI_PASSWORD (none for an open network)
    char wifi_country[3];    ///< WIFI_COUNTRY: two letters, XX worldwide
    char ntp_server[64];     ///< NTP_SERVER
    char tz[64];             ///< TZ as given (a name, a POSIX TZ string or an offset)
    bool mqtt;               ///< MQTT = ON: connect to MQTT_BROKER once Wi-Fi is up
    char mqtt_broker[64];    ///< MQTT_BROKER: a host name or address
    int mqtt_port;           ///< MQTT_PORT (0: 1883)
    char mqtt_user[64];      ///< MQTT_USER
    char mqtt_password[64];  ///< MQTT_PASSWORD
    char mqtt_client_id[33]; ///< MQTT_CLIENT_ID (none: pico-basic- and the end of the MAC)
    int watchdog;            ///< WATCHDOG = ms: armed before AUTORUN.BAS runs (0: not)
    bool clock12;            ///< CLOCK = 12: TIME$ and the clock shown as 1:30:00 PM
} config_t;

extern config_t config;  ///< the settings in force

/// Sets the build's defaults, then reads CONFIG.TXT if it is there. Problems
/// are kept to be shown with the banner (config_warnings).
void config_load(void);

/// Prints the problems CONFIG.TXT had, if any (one line each).
void config_warnings(void);

/// Prints the settings in force (the CONFIG command).
void config_print(void);
