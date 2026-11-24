// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// microSD card on the PiCowbell Adalogger (exploratory). The slot is wired to SPI0:
// GPIO18 SCK, GPIO19 MOSI, GPIO16 MISO, GPIO17 CS. Card detect goes to GPIO15 only if the
// board's SJ1 solder jumper is closed (open from the factory), so it isn't used.
//
// Driver and FAT/exFAT filesystem: carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico (lib/sdcard).

#include "files/sdcard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/spi.h"
#include "pico/stdlib.h"

#include "ff.h"
#include "f_util.h"
#include "hw_config.h"
#include "sd_card.h"

#include "core/log.h"
#include "core/wdog.h"
#include "files/files.h"

// --- hardware configuration (called by the library) ------------------------------------------

static spi_t spi = {
    .hw_inst = spi0,
    .sck_gpio = 18,
    .mosi_gpio = 19,
    .miso_gpio = 16,
    // 25 MHz: the SD spec's maximum in SPI mode. Measured with a 64 GB SDXC card: about
    // 2.2 MB/s write, 1.75 MB/s read (12.5 MHz: 1.2 / 1.0). 37.5 MHz also worked on that card
    // (3.1 / 2.3 MB/s) but is out of spec; 75 MHz fails card init. `sd speed` changes it.
    .baud_rate = 25000 * 1000,
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

static bool mount(shell_t *sh) {
    if (card.state.mounted)
        return true;
    uint64_t t0 = time_us_64();
    FRESULT fr = f_mount(&card.state.fatfs, sd_get_drive_prefix(&card), 1);
    uint32_t ms = (uint32_t)((time_us_64() - t0) / 1000);
    if (fr != FR_OK) {
        if (sh)
            shell_printf(sh, "mount failed: %s (%d) after %lu ms\n", FRESULT_str(fr), fr,
                         (unsigned long)ms);
        else
            log_msg(SOS_LOG_WARNING, "sd: mount failed: %s (%d)", FRESULT_str(fr), fr);
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
    shell_printf(sh, "card:   %s, %llu MB, SPI clock %lu kHz\n", card_type(),
                 (unsigned long long)card_mb, (unsigned long)(spi_get_baudrate(spi.hw_inst) / 1000));
    if (fr != FR_OK) {
        shell_printf(sh, "free:   f_getfree failed: %s\n", FRESULT_str(fr));
        return;
    }
    uint64_t csize = (uint64_t)fs->csize * 512;
    shell_printf(sh, "volume: %s, %llu MB, %llu MB free, cluster %llu bytes\n",
                 fs_type(fs->fs_type), (unsigned long long)((fs->n_fatent - 2) * csize >> 20),
                 (unsigned long long)(free_clusters * csize >> 20), (unsigned long long)csize);
}

// Write then read back a test file, timing both. Feeds the watchdog between blocks.
// Test data: numbered text lines of 64 bytes ("0000123 " + letters + newline), so a kept test
// file can be checked after `cat` or `cp` (line n of the file is test_line(n)).
#define TEST_LINE 64

static void test_line(uint32_t n, char *line) {
    snprintf(line, 9, "%07lu ", (unsigned long)(n % 10000000));
    memset(line + 8, 'a' + (int)(n % 26), TEST_LINE - 9);
    line[TEST_LINE - 1] = '\n';
}

static void cmd_test(shell_t *sh, const char *size_kb, const char *keep) {
    static uint8_t buf[4096];
    const uint32_t lines_per_block = sizeof(buf) / TEST_LINE;
    int blocks = size_kb ? atoi(size_kb) / 4 : 64;     // default 256 KB
    if (blocks < 1 || blocks > 16384 || (keep && strcmp(keep, "keep"))) {
        shell_printf(sh, "usage: sd test [KB [keep]], 4 to 65536 KB; keep leaves /sos-test.txt\n");
        return;
    }
    // One long call: SSH sessions and the console wait until it's done (up to minutes).
    if (!sh->io->admin) {
        shell_printf(sh, "the card test can only be run on the serial console\n");
        return;
    }
    const char *name = "/sos-test.txt";
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
        for (uint32_t l = 0; l < lines_per_block; l++)
            test_line(i * lines_per_block + l, (char *)buf + l * TEST_LINE);
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
        char line[TEST_LINE];
        for (uint32_t l = 0; ok && l < lines_per_block; l++) {
            test_line(i * lines_per_block + l, line);
            ok = !memcmp(buf + l * TEST_LINE, line, TEST_LINE);
        }
        wdog_feed();
    }
    uint64_t rus = time_us_64() - t0;
    f_close(&f);
    FILINFO fi;
    if (f_stat(name, &fi) == FR_OK)
        shell_printf(sh, "test file dated %04u-%02u-%02u %02u:%02u (UTC)\n", (fi.fdate >> 9) + 1980,
                     (fi.fdate >> 5) & 15, fi.fdate & 31, fi.ftime >> 11, (fi.ftime >> 5) & 63);
    if (keep)
        shell_printf(sh, "kept %s\n", name);
    else
        f_unlink(name);
    unsigned kb = (unsigned)(blocks * sizeof(buf) / 1024);
    shell_printf(sh, "write %u KB: %llu ms (%llu KB/s), slowest 4 KB write %llu ms\n", kb,
                 wus / 1000, kb * 1000000ull / wus, worst / 1000);
    shell_printf(sh, "read  %u KB: %llu ms (%llu KB/s), data %s\n", kb, rus / 1000,
                 kb * 1000000ull / rus, ok ? "verified" : "MISMATCH");
}

// Formatting, unmounting or reinitializing the card would break file commands with open files.
static bool files_in_use(shell_t *sh) {
    if (!files_busy())
        return false;
    shell_printf(sh, "files are open (a cat, cp or ls running, or an SFTP transfer); wait for it to end\n");
    return true;
}

bool sdcard_mount(shell_t *sh) {
    if (!driver_init()) {
        if (sh)
            shell_printf(sh, "sd: driver init failed\n");
        return false;
    }
    return mount(sh);
}

// --- SPI clock -----------------------------------------------------------------------------------

// Set the clock used after card initialization (the driver starts at 400 kHz) and reinitialize the
// card so it takes effect. The SPI divider gives clk_peri (150 MHz) / even numbers; the actual
// rate is shown by `sd`.
static void cmd_speed(shell_t *sh, const char *mhz) {
    if (!mhz) {
        shell_printf(sh, "SPI clock: %lu kHz set, %lu kHz actual\n",
                     (unsigned long)(spi.baud_rate / 1000),
                     (unsigned long)(spi_get_baudrate(spi.hw_inst) / 1000));
        return;
    }
    float f = strtof(mhz, NULL);
    if (f < 1 || f > 75) {
        shell_printf(sh, "usage: sd speed [MHz], 1 to 75\n");
        return;
    }
    if (!sh->io->admin) {
        shell_printf(sh, "the SPI clock can only be changed on the serial console\n");
        return;
    }
    if (files_in_use(sh))
        return;
    f_unmount(sd_get_drive_prefix(&card));
    card.state.mounted = false;
    card.state.m_Status |= STA_NOINIT;      // next mount initializes the card at the new rate
    spi.baud_rate = (uint)(f * 1000000);
    if (mount(sh))
        shell_printf(sh, "SPI clock: %lu kHz (asked for %lu)\n",
                     (unsigned long)(spi_get_baudrate(spi.hw_inst) / 1000),
                     (unsigned long)(spi.baud_rate / 1000));
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
    if (files_in_use(sh))
        return;
    void *work = malloc(FORMAT_WORK_BYTES);
    if (!work) {
        shell_printf(sh, "not formatted: out of memory\n");
        return;
    }
    const char *drive = sd_get_drive_prefix(&card);
    f_unmount(drive);
    card.state.mounted = false;
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
    if (files_in_use(sh))
        return;
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
    char *arg2 = arg ? strtok(NULL, " ") : NULL;
    if (!driver_init()) {
        shell_printf(sh, "sd: driver init failed\n");
        return;
    }
    if (!sub || !strcmp(sub, "info"))
        cmd_info(sh);
    else if (!strcmp(sub, "test"))
        cmd_test(sh, arg, arg2);
    else if (!strcmp(sub, "speed"))
        cmd_speed(sh, arg);
    else if (!strcmp(sub, "format"))
        cmd_format(sh, arg);
    else if (!strcmp(sub, "unmount")) {
        if (files_in_use(sh))
            return;
        f_unmount(sd_get_drive_prefix(&card));
        card.state.mounted = false;
        card.state.m_Status |= STA_NOINIT;  // a swapped card is initialized afresh
        shell_printf(sh, "unmounted; safe to remove the card\n");
    } else
        shell_printf(sh, "usage: sd [info|test [KB [keep]]|speed [MHz]|format [exfat|fat32]|unmount]\n");
}
