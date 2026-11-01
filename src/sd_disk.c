/**
 * @file sd_disk.c
 * @brief FatFs's disk functions on the SD card (drive 0, "SD:"), the
 * card's mount, and FatFs's clock.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sd_disk.h"

#include <string.h>
#include <time.h>

#include "board_watchdog.h"
#include "ff.h"
#include "diskio.h"
#include "rpi/emmc.h"

static FATFS fs;

const char *sd_disk_mount(void) {
    const char *err = emmc_init();
    if (err)
        return err;
    FRESULT r = f_mount(&fs, "SD:", 1);
    if (r == FR_NO_FILESYSTEM)
        return "not FAT formatted";
    return r == FR_OK ? NULL : "can't be read";
}

void sd_disk_space(uint32_t *total_kb, uint32_t *free_kb) {
    *total_kb = *free_kb = 0;
    FATFS *f;
    DWORD clusters;
    if (f_getfree("SD:", &clusters, &f) != FR_OK)
        return;
    *total_kb = (uint32_t)((uint64_t)(f->n_fatent - 2) * f->csize / 2);
    *free_kb = (uint32_t)((uint64_t)clusters * f->csize / 2);
}

DSTATUS disk_status(BYTE pdrv) {
    return pdrv == 0 && emmc_ready() ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0)
        return STA_NOINIT;
    // emmc_init has run (sd_disk_mount); a card swapped since needs it again
    if (!emmc_ready() && emmc_init())
        return STA_NOINIT;
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    board_watchdog_feed();  // a long file operation isn't a hang
    if (pdrv != 0)
        return RES_PARERR;
    return emmc_read(buff, (uint32_t)sector, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    board_watchdog_feed();
    if (pdrv != 0)
        return RES_PARERR;
    return emmc_write(buff, (uint32_t)sector, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv != 0)
        return RES_PARERR;
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;  // writes are done when emmc_write returns
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = emmc_sectors();
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = 512;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

DWORD get_fattime(void) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year < 80)  // the clock hasn't been set: 1 Jan 2026
        return (DWORD)(2026 - 1980) << 25 | 1 << 21 | 1 << 16;
    return (DWORD)(tm.tm_year - 80) << 25 | (DWORD)(tm.tm_mon + 1) << 21 | (DWORD)tm.tm_mday << 16 |
           (DWORD)tm.tm_hour << 11 | (DWORD)tm.tm_min << 5 | (DWORD)tm.tm_sec >> 1;
}
