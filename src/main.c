/**
 * @file main.c
 * @brief Pi Zero BASIC: brings the board up (memory, MMU and caches, full ARM
 * clock, serial console, HDMI screen and sound, USB keyboard, SD card), shows the banner, runs AUTORUN.BAS if the
 * card has one, then BASIC's prompt.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>

#include "audio.h"
#include "basic.h"
#include "basic_port.h"
#include "config.h"
#include "dvi.h"
#include "net.h"
#include "terminal.h"
#include "ff.h"
#include "rpi/rpi.h"
#include "rpi/fb.h"
#include "rpi/ticker.h"
#include "screen/console.h"
#include "sd_disk.h"
#include "usb_input.h"

#ifndef PIZERO_BASIC_VERSION
#define PIZERO_BASIC_VERSION "0.9"
#endif

extern char *heap_limit;
uint32_t mbox_arm_full_speed(void);
void __libc_init_array(void);

/// Why the SD card isn't mounted, or NULL; the same for the screen and USB.
static const char *sd_error, *fb_error, *usb_error;
/// Why there is no sound, or NULL (basic_port_rpi.c's MUSIC says it too).
const char *sound_error;

static void board_init(void) {
    uart_init(115200);
    uint32_t base = 0, size = 0;
    if (!mbox_arm_memory(&base, &size) || size == 0)
        size = 0x1C000000;  // 448 MB: a Zero with gpu_mem=64
    heap_limit = (char *)(base + size);
    mmu_init(base + size);
    mbox_arm_full_speed();
    __libc_init_array();
    setvbuf(stdout, NULL, _IONBF, 0);
    uart_enable_irq();
    ticker_init();
    irq_init();
}

static const char *board_name(uint32_t rev) {
    switch (rev & 0xFFFFFF) {
    case 0x9000c1: return "Raspberry Pi Zero W";
    case 0x900092: case 0x900093: case 0x920092: case 0x920093: return "Raspberry Pi Zero";
    default: return "Raspberry Pi (BCM2835)";
    }
}

/// The banner: the version, the board, and the SD card.
static void board_banner(void) {
    uint32_t rev = mbox_board_revision();
    printf("Pi Zero BASIC %s on the %s, %lu MHz\n", PIZERO_BASIC_VERSION, board_name(rev),
           (unsigned long)(mbox_clock_rate(3) / 1000000));
    if (sd_error) {
        printf("SD card: %s; files won't work\n", sd_error);
    } else {
        uint32_t total, free_kb;
        sd_disk_space(&total, &free_kb);
        printf("SD card (SD:): %lu MB, %lu MB free\n", (unsigned long)(total >> 10),
               (unsigned long)(free_kb >> 10));
    }
    if (fb_error) {
        printf("Screen: %s; the serial port only\n", fb_error);
    } else {
        printf("Screen: %dx%d, %dx%d text\n", DVI_WIDTH, DVI_HEIGHT, CONSOLE_COLS, CONSOLE_ROWS);
        printf("Sound: %s\n", sound_error ? sound_error : "HDMI");
    }
    if (usb_error)
        printf("USB: %s\n", usb_error);
    else
        printf("USB keyboard: %s%s\n", usb_keyboard_present() ? "yes" : "none (plug it in before power-up)",
               usb_mouse_present() ? ", mouse: yes" : "");
    config_warnings();
}

/// AUTORUN.BAS on the card, unless Esc or Ctrl-C comes within a second.
static const char *autorun(void) {
    FILINFO info;
    if (sd_error || f_stat("SD:/AUTORUN.BAS", &info) != FR_OK || (info.fattrib & AM_DIR))
        return NULL;
    printf("AUTORUN.BAS (Esc or Ctrl-C now skips it)\n");
    uint64_t end = timer_us() + 1000000;
    while (timer_us() < end) {
        int c = terminal_getc();
        if (c == 3 || c == 27) {
            printf("AUTORUN.BAS skipped\n");
            return NULL;
        }
    }
    printf("Running AUTORUN.BAS; Ctrl-C stops it\n");
    if (config.watchdog) {
        printf("Watchdog on: %d ms (WATCHDOG = in " CONFIG_PATH ")\n", config.watchdog);
        port_watchdog_set(config.watchdog);
    }
    return "SD:/AUTORUN.BAS";
}

int main(void) {
    board_init();
    gpio_write(47, 1);  // the ACT LED off (it is on when low)
    gpio_function(47, GPIO_OUT);
    fb_error = fb_init();
    if (!fb_error)
        terminal_screen_start();
    sound_error = fb_error ? "no screen" : audio_init();
    printf("Pi Zero BASIC: starting USB...\n");
    usb_error = usb_input_init();
    sd_error = sd_disk_mount();
    config_load();
    printf("\x1b[0m\x1b[2J\x1b[H");
    board_banner();
    if (config.wifi && config.wifi_ssid[0] && !sd_error) {
        const char *err = net_connect(config.wifi_ssid, config.wifi_password);
        if (err)
            printf("Wi-Fi: %s\n", err);
        else
            printf("Wi-Fi: joining %s (WIFI says how it goes)\n", config.wifi_ssid);
    }
    const char *first = autorun();
    for (;;) {
        basic_main(first);  // SYSTEM comes back here
        first = NULL;
        printf("\n");
    }
}
