/**
 * @file config.c
 * @brief Implementation of config.h.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "config.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <time.h>

#include "ff.h"
#include "hardware/gpio.h"

// The build's defaults (CMakeLists.txt): the Maker Pi Pico's pins.
#ifndef PICO_BASIC_SD
#define PICO_BASIC_SD 1
#endif
#ifndef SD_SCK
#define SD_SCK 10
#endif
#ifndef SD_MOSI
#define SD_MOSI 11
#endif
#ifndef SD_MISO
#define SD_MISO 12
#endif
#ifndef SD_CS
#define SD_CS 15
#endif
#ifndef SD_DAT1
#define SD_DAT1 13
#endif
#ifndef SD_DAT2
#define SD_DAT2 14
#endif
#ifndef SOUND_PIN
#define SOUND_PIN 18
#endif
#ifndef NEOPIXEL_PIN
#define NEOPIXEL_PIN 28
#endif

config_t config;

#define WARNINGS 6  ///< problems kept to show
static char warnings[WARNINGS][96];
static int nwarnings;

/// Keeps a problem to show with the banner.
/// @param fmt printf style
static void warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void warn(const char *fmt, ...) {
    if (nwarnings == WARNINGS)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(warnings[nwarnings++], sizeof warnings[0], fmt, ap);
    va_end(ap);
}

/// A pin number value; -1 for "-1" or "none"; -2 if it isn't one.
/// @param v the value
/// @return the pin
static int pin_value(const char *v) {
    if (!strcasecmp(v, "none") || !strcmp(v, "-1"))
        return -1;
    if (toupper((unsigned char)v[0]) == 'G' && toupper((unsigned char)v[1]) == 'P')
        v += 2;
    char *end;
    long n = strtol(v, &end, 10);
    if (end == v || *end || n < 0 || n >= NUM_BANK0_GPIOS)
        return -2;
    return (int)n;
}

/// Checks the SD card's pins: SCK, MOSI and MISO of one SPI controller
/// (SCK GP2/6/10/14..., MOSI 3/7/11/15..., MISO 0/4/8/12..., SPI0 or SPI1 by
/// (pin / 8) mod 2), CS any other pin. Turns the card off if they aren't.
static void check_sd_pins(void) {
    const char *why = NULL;
    int spi = (config.sd_sck / 8) & 1;
    if (config.sd_sck % 4 != 2)
        why = "SD_SCK must be an SPI clock pin (GP2, 6, 10, 14, 18, 22...)";
    else if (config.sd_mosi % 4 != 3 || ((config.sd_mosi / 8) & 1) != spi)
        why = "SD_MOSI must be SD_SCK's SPI's TX pin (GP3, 7, 11, 15, 19...)";
    else if (config.sd_miso % 4 != 0 || ((config.sd_miso / 8) & 1) != spi)
        why = "SD_MISO must be SD_SCK's SPI's RX pin (GP0, 4, 8, 12, 16...)";
    else if (config.sd_cs < 0 || config.sd_cs == config.sd_sck || config.sd_cs == config.sd_mosi ||
             config.sd_cs == config.sd_miso)
        why = "SD_CS must be a pin of its own";
    if (why) {
        warn("CONFIG.TXT: %s; SD card off", why);
        config.sd = false;
        config.sd_bad = true;
    }
    config.sd_spi = spi;
}

/// Time zones by name, as POSIX TZ strings (US and EU daylight saving rules).
static const struct {
    const char *name;   ///< TZ's name (any case)
    const char *posix;  ///< the TZ string
} zones[] = {
    {"UTC", "UTC0"}, {"GMT", "GMT0"},
    {"PST", "PST8PDT,M3.2.0,M11.1.0"}, {"PACIFIC", "PST8PDT,M3.2.0,M11.1.0"},
    {"MST", "MST7MDT,M3.2.0,M11.1.0"}, {"MOUNTAIN", "MST7MDT,M3.2.0,M11.1.0"},
    {"ARIZONA", "MST7"},
    {"CST", "CST6CDT,M3.2.0,M11.1.0"}, {"CENTRAL", "CST6CDT,M3.2.0,M11.1.0"},
    {"EST", "EST5EDT,M3.2.0,M11.1.0"}, {"EASTERN", "EST5EDT,M3.2.0,M11.1.0"},
    {"AKST", "AKST9AKDT,M3.2.0,M11.1.0"}, {"ALASKA", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"HST", "HST10"}, {"HAWAII", "HST10"},
    {"UK", "GMT0BST,M3.5.0/1,M10.5.0"}, {"CET", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"EET", "EET-2EEST,M3.5.0/3,M10.5.0/4"}, {"JST", "JST-9"},
    {"AEST", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
};

/// Uses a TZ setting: a name from the list above, an offset from UTC in
/// hours (-5, +5.5), or a POSIX TZ string (EST5EDT,M3.2.0,M11.1.0).
/// @param v the setting
/// @return it was understood
static bool use_tz(const char *v) {
    const char *posix = NULL;
    char buf[64];
    for (size_t i = 0; i < sizeof zones / sizeof *zones && !posix; ++i)
        if (!strcasecmp(v, zones[i].name))
            posix = zones[i].posix;
    char *end;
    double h = strtod(v, &end);
    if (!posix && end != v && !*end && h >= -14 && h <= 14) {
        // POSIX counts the other way: UTC-5 is "UTC5"
        int m = (int)(h * 60 + (h < 0 ? -0.5 : 0.5));
        int a = m < 0 ? -m : m;
        snprintf(buf, sizeof buf, "UTC%s%d:%02d", m > 0 ? "-" : "", a / 60, a % 60);
        posix = buf;
    }
    if (!posix) {
        if (!isalpha((unsigned char)v[0]) || !isalpha((unsigned char)v[1]) ||
            !isalpha((unsigned char)v[2]))
            return false;  // a POSIX TZ starts with its zone's name
        posix = v;
    }
    setenv("TZ", posix, 1);
    tzset();
    return true;
}

/// Copies a text setting, warning if it's too long.
/// @param line the line's number
/// @param key the key
/// @param v its value
/// @param out where it goes
/// @param size room in out
static void set_text(int line, const char *key, const char *v, char *out, size_t size) {
    if (strlen(v) >= size)
        warn("CONFIG.TXT line %d: %s is too long (%u at most)", line, key, (unsigned)size - 1);
    else
        strcpy(out, v);
}

/// Sets one setting from a KEY = value line.
/// @param line the line's number (for messages)
/// @param key the key
/// @param v its value
static void set(int line, const char *key, const char *v) {
    int *pin = !strcasecmp(key, "SD_SCK")    ? &config.sd_sck
               : !strcasecmp(key, "SD_MOSI") ? &config.sd_mosi
               : !strcasecmp(key, "SD_MISO") ? &config.sd_miso
               : !strcasecmp(key, "SD_CS")   ? &config.sd_cs
               : !strcasecmp(key, "SPEAKER") ? &config.speaker
               : !strcasecmp(key, "NEOPIXEL") ? &config.neopixel
                                             : NULL;
    if (pin) {
        int p = pin_value(v);
        if (p == -2 || (p == -1 && pin != &config.speaker))
            warn("CONFIG.TXT line %d: %s = %s isn't a pin", line, key, v);
        else
            *pin = p;
    } else if (!strcasecmp(key, "SD")) {
        if (!strcasecmp(v, "ON") || !strcasecmp(v, "YES") || !strcmp(v, "1"))
            config.sd = true;
        else if (!strcasecmp(v, "OFF") || !strcasecmp(v, "NO") || !strcmp(v, "0"))
            config.sd = false;
        else
            warn("CONFIG.TXT line %d: SD = %s: ON or OFF", line, v);
    } else if (!strcasecmp(key, "WIFI")) {
        if (!strcasecmp(v, "ON") || !strcasecmp(v, "YES") || !strcmp(v, "1"))
            config.wifi = true;
        else if (!strcasecmp(v, "OFF") || !strcasecmp(v, "NO") || !strcmp(v, "0"))
            config.wifi = false;
        else
            warn("CONFIG.TXT line %d: WIFI = %s: ON or OFF", line, v);
    } else if (!strcasecmp(key, "WIFI_SSID")) {
        set_text(line, key, v, config.wifi_ssid, sizeof config.wifi_ssid);
    } else if (!strcasecmp(key, "WIFI_PASSWORD")) {
        set_text(line, key, v, config.wifi_password, sizeof config.wifi_password);
    } else if (!strcasecmp(key, "WIFI_COUNTRY")) {
        if (strlen(v) == 2 && isalpha((unsigned char)v[0]) && isalpha((unsigned char)v[1])) {
            config.wifi_country[0] = toupper((unsigned char)v[0]);
            config.wifi_country[1] = toupper((unsigned char)v[1]);
            config.wifi_country[2] = 0;
        } else {
            warn("CONFIG.TXT line %d: WIFI_COUNTRY = %s: two letters (US, GB, DE...; XX: anywhere)",
                 line, v);
        }
    } else if (!strcasecmp(key, "NTP_SERVER")) {
        set_text(line, key, v, config.ntp_server, sizeof config.ntp_server);
    } else if (!strcasecmp(key, "MQTT")) {
        if (!strcasecmp(v, "ON") || !strcasecmp(v, "YES") || !strcmp(v, "1"))
            config.mqtt = true;
        else if (!strcasecmp(v, "OFF") || !strcasecmp(v, "NO") || !strcmp(v, "0"))
            config.mqtt = false;
        else
            warn("CONFIG.TXT line %d: MQTT = %s: ON or OFF", line, v);
    } else if (!strcasecmp(key, "MQTT_BROKER")) {
        set_text(line, key, v, config.mqtt_broker, sizeof config.mqtt_broker);
    } else if (!strcasecmp(key, "MQTT_PORT")) {
        char *end;
        long n = strtol(v, &end, 10);
        if (end == v || *end || n < 1 || n > 65535)
            warn("CONFIG.TXT line %d: MQTT_PORT = %s: 1 to 65535", line, v);
        else
            config.mqtt_port = (int)n;
    } else if (!strcasecmp(key, "MQTT_USER")) {
        set_text(line, key, v, config.mqtt_user, sizeof config.mqtt_user);
    } else if (!strcasecmp(key, "MQTT_PASSWORD")) {
        set_text(line, key, v, config.mqtt_password, sizeof config.mqtt_password);
    } else if (!strcasecmp(key, "MQTT_CLIENT_ID")) {
        set_text(line, key, v, config.mqtt_client_id, sizeof config.mqtt_client_id);
    } else if (!strcasecmp(key, "WATCHDOG")) {
        char *end;
        long ms = strtol(v, &end, 10);
        if (!strcasecmp(v, "OFF") || !strcmp(v, "0"))
            config.watchdog = 0;
        else if (end != v && !*end && ms >= 100 && ms <= 8388)
            config.watchdog = (int)ms;
        else
            warn("CONFIG.TXT line %d: WATCHDOG = %s: 100 to 8388 ms, or OFF", line, v);
    } else if (!strcasecmp(key, "CLOCK")) {
        if (!strcmp(v, "12") || !strcmp(v, "24") || !strcasecmp(v, "12H") || !strcasecmp(v, "24H"))
            config.clock12 = v[0] == '1';
        else
            warn("CONFIG.TXT line %d: CLOCK = %s: 12 or 24", line, v);
    } else if (!strcasecmp(key, "TZ")) {
        if (strlen(v) >= sizeof config.tz || !use_tz(v))
            warn("CONFIG.TXT line %d: TZ = %s: a name (EST, PST, UK, CET...), hours from UTC (-5), "
                 "or a POSIX TZ string", line, v);
        else
            strcpy(config.tz, v);
    } else if (!strcasecmp(key, "SD_MODE")) {
        if (strlen(v) == 1 && v[0] >= '0' && v[0] <= '3')
            config.sd_mode = v[0] - '0';
        else
            warn("CONFIG.TXT line %d: SD_MODE = %s: 0 to 3", line, v);
    } else if (!strcasecmp(key, "SD_PULLUPS")) {
        config.sd_npullups = 0;
        char buf[64];
        snprintf(buf, sizeof buf, "%s", v);
        for (char *t = strtok(buf, ", "); t; t = strtok(NULL, ", ")) {
            int p = pin_value(t);
            if (p == -1)
                continue;  // "none"
            if (p < 0 || config.sd_npullups == CONFIG_PULLUPS)
                warn("CONFIG.TXT line %d: SD_PULLUPS: %s isn't a pin (or too many)", line, t);
            else
                config.sd_pullups[config.sd_npullups++] = p;
        }
    } else {
        warn("CONFIG.TXT line %d: %s isn't a setting (ignored)", line, key);
    }
}

void config_load(void) {
    config = (config_t){
        .sd = PICO_BASIC_SD, .sd_sck = SD_SCK, .sd_mosi = SD_MOSI, .sd_miso = SD_MISO,
        .sd_cs = SD_CS, .sd_mode = 3, .speaker = SOUND_PIN, .neopixel = NEOPIXEL_PIN,
        .wifi = true, .wifi_country = "XX", .ntp_server = "pool.ntp.org",
    };
    if (SD_DAT1 >= 0)
        config.sd_pullups[config.sd_npullups++] = SD_DAT1;
    if (SD_DAT2 >= 0)
        config.sd_pullups[config.sd_npullups++] = SD_DAT2;
    nwarnings = 0;
    unsetenv("TZ");  // UTC unless CONFIG.TXT says
    tzset();
    FIL f;
    if (f_open(&f, CONFIG_PATH, FA_READ) == FR_OK) {
        config.from_file = true;
        char line[128];
        for (int n = 1; f_gets(line, sizeof line, &f); ++n) {
            char *c = strpbrk(line, "#';\r\n");  // the comment, or the end
            if (c)
                *c = 0;
            char *eq = strchr(line, '=');
            char *key = line;
            while (isspace((unsigned char)*key))
                ++key;
            if (!*key)
                continue;  // blank or a comment
            if (!eq) {
                warn("CONFIG.TXT line %d: KEY = value, please", n);
                continue;
            }
            *eq = 0;
            char *v = eq + 1, *e = eq;
            while (e > key && isspace((unsigned char)e[-1]))
                *--e = 0;
            while (isspace((unsigned char)*v))
                ++v;
            for (e = v + strlen(v); e > v && isspace((unsigned char)e[-1]);)
                *--e = 0;
            set(n, key, v);
        }
        f_close(&f);
    }
#ifndef PIZERO
    if (config.sd)
        check_sd_pins();
#endif
    if (config.speaker >= 0 && config.sd &&
        (config.speaker == config.sd_sck || config.speaker == config.sd_mosi ||
         config.speaker == config.sd_miso || config.speaker == config.sd_cs)) {
        warn("CONFIG.TXT: SPEAKER is one of the SD card's pins; no speaker");
        config.speaker = -1;
    }
}

void config_warnings(void) {
    for (int i = 0; i < nwarnings; ++i)
        printf("%s\n", warnings[i]);
}

void config_print(void) {
    printf("Settings (%s):\n", config.from_file ? CONFIG_PATH : "the build's; no " CONFIG_PATH);
#ifdef PIZERO
    printf("  SD: the card slot (the only drive)\n");
    if (0) {
#else
    if (config.sd) {
#endif
        printf("  SD = ON: SPI%d, SCK GP%d, MOSI GP%d, MISO GP%d, CS GP%d, mode %d", config.sd_spi,
               config.sd_sck, config.sd_mosi, config.sd_miso, config.sd_cs, config.sd_mode);
        for (int i = 0; i < config.sd_npullups; ++i)
            printf("%s GP%d", i ? "," : "; pull-ups", config.sd_pullups[i]);
        printf("\n");
    } else {
        printf("  SD = OFF\n");
    }
    if (config.speaker >= 0)
        printf("  SPEAKER = GP%d\n", config.speaker);
    else
        printf("  SPEAKER = none\n");
    printf("  NEOPIXEL = GP%d (NEOPIXEL OPEN's default)\n", config.neopixel);
#if PICO_BASIC_WIFI
    if (config.wifi_ssid[0]) {
        printf("  WIFI = %s: WIFI_SSID %s, WIFI_PASSWORD %s, WIFI_COUNTRY %s\n",
               config.wifi ? "ON" : "OFF", config.wifi_ssid,
               config.wifi_password[0] ? "****" : "none", config.wifi_country);
        printf("  NTP_SERVER = %s\n", config.ntp_server);
        if (config.mqtt_broker[0])
            printf("  MQTT = %s: MQTT_BROKER %s, MQTT_PORT %d%s%s%s%s%s\n", config.mqtt ? "ON" : "OFF",
                   config.mqtt_broker, config.mqtt_port ? config.mqtt_port : 1883,
                   config.mqtt_user[0] ? ", MQTT_USER " : "", config.mqtt_user,
                   config.mqtt_password[0] ? ", MQTT_PASSWORD ****" : "",
                   config.mqtt_client_id[0] ? ", MQTT_CLIENT_ID " : "", config.mqtt_client_id);
    } else {
        printf("  WIFI: no WIFI_SSID\n");
    }
#else
    if (config.wifi_ssid[0])
        printf("  WIFI_SSID = %s (this build has no Wi-Fi)\n", config.wifi_ssid);
#endif
    printf("  TZ = %s\n", config.tz[0] ? config.tz : "UTC (none set)");
    if (config.clock12)
        printf("  CLOCK = 12 (TIME$ 01:30:00 PM)\n");
    if (config.watchdog)
        printf("  WATCHDOG = %d (armed before AUTORUN.BAS)\n", config.watchdog);
    config_warnings();
}
