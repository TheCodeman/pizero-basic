/**
 * @file main.c
 * @brief Pi Zero BASIC, step 1: bring the board up (memory, MMU and caches,
 * full ARM clock, serial console) and run a line-echo loop that proves the
 * console, the C library, floating point and the heap all work.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "rpi/rpi.h"

#ifndef PIZERO_BASIC_VERSION
#define PIZERO_BASIC_VERSION "0.1"
#endif

extern char *heap_limit;
extern char __heap_start[];
size_t heap_room(void);
uint32_t mbox_arm_full_speed(void);
void __libc_init_array(void);

static void board_init(void) {
    uart_init(115200);

    uint32_t base = 0, size = 0;
    if (!mbox_arm_memory(&base, &size) || size == 0)
        size = 0x1C000000;  // 448 MB: a Zero with gpu_mem=64
    uint32_t top = base + size;
    heap_limit = (char *)top;
    mmu_init(top);
    mbox_arm_full_speed();
    __libc_init_array();
    setvbuf(stdout, NULL, _IONBF, 0);
}

/// Reads a line with backspace; returns its length.
static int read_line(char *buf, int size) {
    int n = 0;
    for (;;) {
        int c = console_getc_wait();
        if (c == '\r' || c == '\n') {
            fputs("\n", stdout);
            buf[n] = 0;
            return n;
        }
        if ((c == 8 || c == 127) && n > 0) {
            n--;
            fputs("\b \b", stdout);
        } else if (c >= 32 && c < 127 && n < size - 1) {
            buf[n++] = (char)c;
            putchar(c);
        }
    }
}

int main(void) {
    board_init();

    uint32_t rev = mbox_board_revision();
    printf("\nPi Zero BASIC %s (step 1: console)\n", PIZERO_BASIC_VERSION);
    printf("Board revision %06lx%s, ARM %lu MHz, %lu MB for the ARM, heap %lu MB\n",
           (unsigned long)rev, rev == 0x9000c1 ? " (Zero W)" : "",
           (unsigned long)(mbox_clock_rate(3) / 1000000),
           (unsigned long)((uint32_t)(uintptr_t)heap_limit >> 20),
           (unsigned long)(heap_room() >> 20));

    // A quick check that floating point and malloc work.
    double *d = malloc(1000 * sizeof *d);
    double sum = 0;
    for (int i = 0; i < 1000; i++) {
        d[i] = 1.0 / (i + 1);
        sum += d[i];
    }
    free(d);
    printf("H(1000) = %.6f (should be 7.485471)\n", sum);
    uint32_t t = mbox_temperature();
    if (t)
        printf("SoC temperature %.1f C\n", t / 1000.0);
    printf("Type a line and it comes back; REBOOT restarts.\n");

    char line[256];
    for (;;) {
        fputs("> ", stdout);
        read_line(line, sizeof line);
        if (strcmp(line, "REBOOT") == 0 || strcmp(line, "reboot") == 0)
            board_reboot();
        uint64_t t0 = timer_us();
        printf("%s  [%u chars, %.3f s up]\n", line, (unsigned)strlen(line), t0 / 1e6);
    }
}
