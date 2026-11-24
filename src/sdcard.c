// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// microSD card on the PiCowbell Adalogger (exploratory). The slot is wired to SPI0:
// GPIO18 SCK, GPIO19 MOSI, GPIO16 MISO, GPIO17 CS. Card detect goes to GPIO15 only if the
// board's SJ1 solder jumper is closed (open from the factory), so it isn't used.
//
// Driver and FAT/exFAT filesystem: carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico (lib/sdcard).

#include "sdcard.h"

#include <string.h>
#include <sys/time.h>

#include "pico/aon_timer.h"
#include "pico/stdlib.h"

#include "ff.h"
#include "f_util.h"
#include "hw_config.h"
#include "sd_card.h"

#include "log.h"
#include "wdog.h"

// --- hardware configuration (called by the library) ------------------------------------------

static spi_t spi = {
    .hw_inst = spi0,
    .sck_gpio = 18,
    .mosi_gpio = 19,
    .miso_gpio = 16,
    .baud_rate = 12500 * 1000,          // conservative for a first try
};

static sd_spi_if_t spi_if = {
    .spi = &spi,
    .ss_gpio = 17,
};

static sd_card_t card = {
    .type = SD_IF_SPI,
    .spi_if_p = &spi_if,
    .use_card_detect = false,
};

size_t sd_get_num(void) { return 1; }
sd_card_t *sd_get_by_num(size_t num) { return num == 0 ? &card : NULL; }

// The library's messages (weak hooks, silent by default) go to the system log.
void put_out_error_message(const char *s) { log_msg(SOS_LOG_ERR, "sd: %s", s); }
void put_out_info_message(const char *s) { log_msg(SOS_LOG_INFO, "sd: %s", s); }

// --- shell command -----------------------------------------------------------------------------

static const char *card_type(void) {
    switch (card.state.card_type) {
    case SDCARD_V1:   return "SDSC (v1)";
    case SDCARD_V2:   return "SDSC (v2)";
    case SDCARD_V2HC: return "SDHC/SDXC";
    default:          return "unknown";
    }
}

static const char *fs_type(BYTE t) {
    switch (t) {
    case FS_FAT12: return "FAT12";
    case FS_FAT16: return "FAT16";
    case FS_FAT32: return "FAT32";
    case FS_EXFAT: return "exFAT";
    default:       return "?";
    }
}

// The library dates files from the always-on timer (get_fattime), while SOS keeps the time in
// gettimeofday() (set from the RTC or NTP). Copy it over before anything is written; without a
// valid time the files get FatFs's default date.
static void sync_fat_clock(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec < 1704067200)             // before 2024: the clock isn't set
        return;
    struct timespec ts = { .tv_sec = tv.tv_sec, .tv_nsec = tv.tv_usec * 1000 };
    if (aon_timer_is_running())
        aon_timer_set_time(&ts);
    else
        aon_timer_start(&ts);
}

static bool mount(shell_t *sh) {
    sync_fat_clock();
    if (card.state.mounted)
        return true;
    uint64_t t0 = time_us_64();
    FRESULT fr = f_mount(&card.state.fatfs, sd_get_drive_prefix(&card), 1);
    uint32_t ms = (uint32_t)((time_us_64() - t0) / 1000);
    if (fr != FR_OK) {
        shell_printf(sh, "mount failed: %s (%d) after %lu ms\n", FRESULT_str(fr), fr,
                     (unsigned long)ms);
        return false;
    }
    card.state.mounted = true;
    log_msg(SOS_LOG_NOTICE, "sd: mounted %s in %lu ms", fs_type(card.state.fatfs.fs_type),
            (unsigned long)ms);
    return true;
}

static void cmd_info(shell_t *sh) {
    if (!mount(sh))
        return;
    FATFS *fs;
    DWORD free_clusters;
    FRESULT fr = f_getfree(sd_get_drive_prefix(&card), &free_clusters, &fs);
    uint64_t card_mb = (uint64_t)card.state.sectors * 512 / (1024 * 1024);
    shell_printf(sh, "card:   %s, %llu MB\n", card_type(), (unsigned long long)card_mb);
    if (fr != FR_OK) {
        shell_printf(sh, "free:   f_getfree failed: %s\n", FRESULT_str(fr));
        return;
    }
    uint64_t csize = (uint64_t)fs->csize * 512;
    shell_printf(sh, "volume: %s, %llu MB, %llu MB free, cluster %llu bytes\n",
                 fs_type(fs->fs_type), (unsigned long long)((fs->n_fatent - 2) * csize >> 20),
                 (unsigned long long)(free_clusters * csize >> 20), (unsigned long long)csize);
}

static void cmd_ls(shell_t *sh, const char *path) {
    if (!mount(sh))
        return;
    DIR dir;
    FILINFO fi;
    FRESULT fr = f_opendir(&dir, path ? path : "/");
    if (fr != FR_OK) {
        shell_printf(sh, "ls: %s\n", FRESULT_str(fr));
        return;
    }
    int n = 0;
    while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fattrib & AM_DIR)
            shell_printf(sh, "%10s  %s/\n", "<dir>", fi.fname);
        else
            shell_printf(sh, "%10llu  %04u-%02u-%02u %02u:%02u  %s\n",
                         (unsigned long long)fi.fsize, (fi.fdate >> 9) + 1980,
                         (fi.fdate >> 5) & 15, fi.fdate & 31, fi.ftime >> 11,
                         (fi.ftime >> 5) & 63, fi.fname);
        n++;
    }
    f_closedir(&dir);
    shell_printf(sh, "%d entries\n", n);
}

// Write then read back a test file, timing both. Feeds the watchdog between blocks.
static void cmd_test(shell_t *sh) {
    static uint8_t buf[4096];
    const int blocks = 64;                  // 256 KB
    const char *name = "/sos-test.bin";
    if (!mount(sh))
        return;
    FIL f;
    FRESULT fr = f_open(&f, name, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) {
        shell_printf(sh, "open for write: %s\n", FRESULT_str(fr));
        return;
    }
    uint64_t t0 = time_us_64(), worst = 0;
    for (int i = 0; i < blocks && fr == FR_OK; i++) {
        memset(buf, i, sizeof(buf));
        UINT bw;
        uint64_t b0 = time_us_64();
        fr = f_write(&f, buf, sizeof(buf), &bw);
        if (time_us_64() - b0 > worst)
            worst = time_us_64() - b0;
        if (fr == FR_OK && bw != sizeof(buf))
            fr = FR_DENIED;                 // volume full
        wdog_feed();
    }
    FRESULT cr = f_close(&f);
    if (fr == FR_OK)
        fr = cr;
    uint64_t wus = time_us_64() - t0;
    if (fr != FR_OK) {
        shell_printf(sh, "write: %s\n", FRESULT_str(fr));
        return;
    }
    fr = f_open(&f, name, FA_READ);
    t0 = time_us_64();
    bool ok = fr == FR_OK;
    for (int i = 0; i < blocks && ok; i++) {
        UINT br;
        ok = f_read(&f, buf, sizeof(buf), &br) == FR_OK && br == sizeof(buf);
        for (size_t j = 0; ok && j < sizeof(buf); j++)
            ok = buf[j] == (uint8_t)i;
        wdog_feed();
    }
    uint64_t rus = time_us_64() - t0;
    f_close(&f);
    FILINFO fi;
    if (f_stat(name, &fi) == FR_OK)
        shell_printf(sh, "test file dated %04u-%02u-%02u %02u:%02u (UTC)\n", (fi.fdate >> 9) + 1980,
                     (fi.fdate >> 5) & 15, fi.fdate & 31, fi.ftime >> 11, (fi.ftime >> 5) & 63);
    f_unlink(name);
    unsigned kb = blocks * sizeof(buf) / 1024;
    shell_printf(sh, "write %u KB: %llu ms (%llu KB/s), slowest 4 KB write %llu ms\n", kb,
                 wus / 1000, kb * 1000000ull / wus, worst / 1000);
    shell_printf(sh, "read  %u KB: %llu ms (%llu KB/s), data %s\n", kb, rus / 1000,
                 kb * 1000000ull / rus, ok ? "verified" : "MISMATCH");
}

void sdcard_command(shell_t *sh, char *args) {
    char *sub = args ? strtok(args, " ") : NULL;
    char *arg = sub ? strtok(NULL, " ") : NULL;
    static bool driver_ready;
    if (!driver_ready)
        driver_ready = sd_init_driver();
    if (!sub || !strcmp(sub, "info"))
        cmd_info(sh);
    else if (!strcmp(sub, "ls"))
        cmd_ls(sh, arg);
    else if (!strcmp(sub, "test"))
        cmd_test(sh);
    else if (!strcmp(sub, "unmount")) {
        f_unmount(sd_get_drive_prefix(&card));
        card.state.mounted = false;
        shell_printf(sh, "unmounted\n");
    } else
        shell_printf(sh, "usage: sd [info|ls [path]|test|unmount]\n");
}
