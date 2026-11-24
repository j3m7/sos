// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// microSD card on the PiCowbell Adalogger (exploratory). The slot is wired to SPI0:
// GPIO18 SCK, GPIO19 MOSI, GPIO16 MISO, GPIO17 CS. Card detect goes to GPIO15 only if the
// board's SJ1 solder jumper is closed (open from the factory), so it isn't used.
//
// Driver and FAT/exFAT filesystem: carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico (lib/sdcard).

#include "sdcard.h"

#include <stdlib.h>
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

// While formatting (one long FatFs call), feed the watchdog from each block write: the main loop
// doesn't run until it returns, but every completed write is progress.
static bool formatting;
static block_dev_err_t (*lib_write_blocks)(sd_card_t *, const uint8_t *, uint32_t, uint32_t);
static block_dev_err_t (*lib_read_blocks)(sd_card_t *, uint8_t *, uint32_t, uint32_t);
static uint32_t (*lib_get_num_sectors)(sd_card_t *);

// FatFs asks for the card size again at the end of a format, right after a long write, when the
// card can still be busy: the driver's CMD9 (read CSD) then fails and it reports 0 sectors, which
// fails the format. The size can't change while the card stays initialized (init stores it), so
// fall back to that.
static uint32_t get_num_sectors_cached(sd_card_t *c) {
    uint32_t n = lib_get_num_sectors(c);
    if (!n && c->state.sectors) {
        log_msg(SOS_LOG_INFO, "sd: size query failed, using %lu sectors from init",
                (unsigned long)c->state.sectors);
        n = c->state.sectors;
    }
    return n;
}

static struct { uint32_t writes, reads, sectors, last_sector, last_count; } io_stats;

static block_dev_err_t read_blocks_logged(sd_card_t *c, uint8_t *buf, uint32_t sector,
                                          uint32_t count) {
    block_dev_err_t r = lib_read_blocks(c, buf, sector, count);
    io_stats.reads++;
    if (r != SD_BLOCK_DEVICE_ERROR_NONE)
        log_msg(SOS_LOG_ERR, "sd: reading %lu sectors at %lu failed (%d)", (unsigned long)count,
                (unsigned long)sector, (int)r);
    return r;
}

static block_dev_err_t write_blocks_fed(sd_card_t *c, const uint8_t *buf, uint32_t sector,
                                        uint32_t count) {
    block_dev_err_t r = lib_write_blocks(c, buf, sector, count);
    io_stats.writes++;
    io_stats.sectors += count;
    io_stats.last_sector = sector;
    io_stats.last_count = count;
    if (formatting)
        wdog_feed();
    if (r != SD_BLOCK_DEVICE_ERROR_NONE)
        log_msg(SOS_LOG_ERR, "sd: writing %lu sectors at %lu failed (%d)", (unsigned long)count,
                (unsigned long)sector, (int)r);
    return r;
}

static bool driver_init(void) {
    static bool ready;
    if (!ready && sd_init_driver()) {
        lib_write_blocks = card.write_blocks;   // set by sd_init_driver()
        card.write_blocks = write_blocks_fed;
        lib_read_blocks = card.read_blocks;
        card.read_blocks = read_blocks_logged;
        lib_get_num_sectors = card.get_num_sectors;
        card.get_num_sectors = get_num_sectors_cached;
        ready = true;
    }
    return ready;
}

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

// --- format --------------------------------------------------------------------------------------

#define FORMAT_ALIGN_SECTORS 8192           // 4 MB: a multiple of the card's allocation unit
#define FORMAT_WORK_BYTES    (32 * 1024)    // bigger is faster (f_mkfs writes this much at once)

static BYTE format_type;                    // FM_EXFAT or FM_FAT32

static void on_format_confirm(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (!answer || strcmp(answer, "yes")) {
        shell_printf(sh, "not formatted\n");
        return;
    }
    void *work = malloc(FORMAT_WORK_BYTES);
    if (!work) {
        shell_printf(sh, "not formatted: out of memory\n");
        return;
    }
    const char *drive = sd_get_drive_prefix(&card);
    f_unmount(drive);
    card.state.mounted = false;
    sync_fat_clock();                       // volume and directory dates
    const char *name = format_type == FM_EXFAT ? "exFAT" : "FAT32";
    log_msg(SOS_LOG_NOTICE, "sd: formatting the card as %s", name);

    MKFS_PARM opt = { .fmt = format_type, .n_fat = 1, .align = FORMAT_ALIGN_SECTORS };
    uint64_t t0 = time_us_64();
    memset(&io_stats, 0, sizeof(io_stats));
    formatting = true;
    // No FM_SFD: writes a new partition table with one partition over the whole card.
    FRESULT fr = f_mkfs(drive, &opt, work, FORMAT_WORK_BYTES);
    formatting = false;
    uint32_t ms = (uint32_t)((time_us_64() - t0) / 1000);
    free(work);
    log_msg(SOS_LOG_INFO, "sd: format did %lu reads, %lu writes (%lu sectors), last write %lu+%lu",
            (unsigned long)io_stats.reads, (unsigned long)io_stats.writes,
            (unsigned long)io_stats.sectors, (unsigned long)io_stats.last_sector,
            (unsigned long)io_stats.last_count);
    if (fr != FR_OK) {
        log_msg(SOS_LOG_ERR, "sd: format failed: %s (%d) after %lu ms", FRESULT_str(fr), fr,
                (unsigned long)ms);
        shell_printf(sh, "format failed: %s\n", FRESULT_str(fr));
        return;
    }
    log_msg(SOS_LOG_NOTICE, "sd: formatted as %s in %lu ms", name, (unsigned long)ms);
    if (!mount(sh))
        return;
    shell_printf(sh, "formatted in %lu.%lu s\n", (unsigned long)(ms / 1000),
                 (unsigned long)(ms % 1000 / 100));
    cmd_info(sh);
}

static void cmd_format(shell_t *sh, const char *type) {
    if (!sh->io->admin) {
        shell_printf(sh, "the card can only be formatted on the serial console\n");
        return;
    }
    if (!type || !strcmp(type, "exfat"))
        format_type = FM_EXFAT;
    else if (!strcmp(type, "fat32"))
        format_type = FM_FAT32;
    else {
        shell_printf(sh, "usage: sd format [exfat|fat32]\n");
        return;
    }

    // Make sure there's a card, and show what's on it now.
    if (card.state.mounted || mount(sh))
        cmd_info(sh);
    else if (!card.state.sectors) {
        shell_printf(sh, "no card\n");
        return;
    }
    // One-line prompt: log lines redraw only the input line.
    shell_printf(sh, "This erases the whole card (all partitions) and formats it as %s.\n"
                 "SSH sessions pause until it's done.\n",
                 format_type == FM_EXFAT ? "exFAT" : "FAT32");
    shell_ask(sh, "Erase the card? Type 'yes': ", false, on_format_confirm, NULL);
}

// --- dispatch ------------------------------------------------------------------------------------

void sdcard_command(shell_t *sh, char *args) {
    char *sub = args ? strtok(args, " ") : NULL;
    char *arg = sub ? strtok(NULL, " ") : NULL;
    if (!driver_init()) {
        shell_printf(sh, "sd: driver init failed\n");
        return;
    }
    if (!sub || !strcmp(sub, "info"))
        cmd_info(sh);
    else if (!strcmp(sub, "ls"))
        cmd_ls(sh, arg);
    else if (!strcmp(sub, "test"))
        cmd_test(sh);
    else if (!strcmp(sub, "format"))
        cmd_format(sh, arg);
    else if (!strcmp(sub, "unmount")) {
        f_unmount(sd_get_drive_prefix(&card));
        card.state.mounted = false;
        shell_printf(sh, "unmounted\n");
    } else
        shell_printf(sh, "usage: sd [info|ls [path]|test|format [exfat|fat32]|unmount]\n");
}
