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

#include "diskio.h"
#include "ff.h"
#include "f_util.h"
#include "hw_config.h"
#include "sd_card.h"

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/random.h>

#include "core/heaptag.h"
#include "core/log.h"
#include "core/wdog.h"
#include "files/files.h"
#include "files/cardkey.h"
#include "files/sdcrypt.h"

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

// The library's messages (weak hooks, silent by default) go to the system log, except during a
// quiet mount (sdcard_mount_retry()): a missing card would otherwise say so every minute.
static bool quiet;
void put_out_error_message(const char *s) {
    if (!quiet)
        log_msg(SOS_LOG_DAEMON | SOS_LOG_ERR, "sd: %s", s);
}
void put_out_info_message(const char *s) {
    if (!quiet)
        log_msg(SOS_LOG_DAEMON | SOS_LOG_INFO, "sd: %s", s);
}

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
        log_msg(SOS_LOG_DAEMON | SOS_LOG_INFO, "sd: size query failed, using %lu sectors from init",
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
        log_msg(SOS_LOG_DAEMON | SOS_LOG_ERR,
                "sd: reading %lu sectors at %lu failed (%d)", (unsigned long)count,
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
        log_msg(SOS_LOG_DAEMON | SOS_LOG_ERR,
                "sd: writing %lu sectors at %lu failed (%d)", (unsigned long)count,
                (unsigned long)sector, (int)r);
    return r;
}

// --- encryption (see files/sdcrypt.h) ----------------------------------------------------------
//
// Between FatFs and the driver: on an encrypted card, FatFs's sector n is the card's sector
// n + SDCRYPT_OFFSET, decrypted after reading and encrypted before writing. The kind of card is
// found at each mount (detect()).

static enum {
    KIND_UNKNOWN,           // not mounted since the card was (re)initialized
    KIND_PLAIN,
    KIND_ENCRYPTED,         // the layer is on
    KIND_LOCKED,            // encrypted, but this board's card key doesn't open it (or there is none)
    KIND_UNSUPPORTED,       // encrypted in a format this firmware doesn't know
} card_kind;
static uint8_t card_id[SDCRYPT_ID_LEN];

// The layer's working memory, 7.5 KB, on the heap (HEAP_FILES) only while it's needed: for the
// moment it takes to look at a card's header, and from then on if the card is encrypted, until
// it is unmounted, taken out or reformatted. A board with a plain card, or none, doesn't hold
// it. KIND_ENCRYPTED implies it is there.
typedef struct {
    sdcrypt_t crypt;
    uint8_t buf[8 * SDCRYPT_SECTOR];        // sectors encrypted before writing; header sectors
} crypt_state_t;
static crypt_state_t *cs;

static bool crypt_alloc(void) {
    if (!cs)
        cs = heap_zalloc(HEAP_FILES, sizeof(*cs));
    return cs != NULL;
}

static void crypt_free(void) {
    if (!cs)
        return;
    sdcrypt_stop(&cs->crypt);
    sdcrypt_wipe(cs, sizeof(*cs));          // it held the card's key
    heap_release(HEAP_FILES, cs);
    cs = NULL;
}

static bool repair_header;      // opened from the backup: rewrite sector 0 once the volume mounts

static void crypt_off(void) {
    crypt_free();
    card_kind = KIND_UNKNOWN;
    repair_header = false;
}

static block_dev_err_t read_blocks_layer(sd_card_t *c, uint8_t *buf, uint32_t sector,
                                         uint32_t count) {
    if (card_kind == KIND_LOCKED || card_kind == KIND_UNSUPPORTED)
        return SD_BLOCK_DEVICE_ERROR_NO_DEVICE;
    if (card_kind != KIND_ENCRYPTED)
        return read_blocks_logged(c, buf, sector, count);
    block_dev_err_t r = read_blocks_logged(c, buf, sector + SDCRYPT_OFFSET, count);
    if (r == SD_BLOCK_DEVICE_ERROR_NONE && !sdcrypt_decrypt(&cs->crypt, buf, sector, count))
        r = SD_BLOCK_DEVICE_ERROR_UNSUPPORTED;
    return r;
}

static block_dev_err_t write_blocks_layer(sd_card_t *c, const uint8_t *buf, uint32_t sector,
                                          uint32_t count) {
    if (card_kind == KIND_LOCKED || card_kind == KIND_UNSUPPORTED)
        return SD_BLOCK_DEVICE_ERROR_NO_DEVICE;
    if (card_kind != KIND_ENCRYPTED)
        return write_blocks_fed(c, buf, sector, count);
    // The caller's buffer (FatFs's window, or the caller's data) must not change: encrypt into
    // cs->buf, a chunk at a time.
    const uint32_t chunk = sizeof(cs->buf) / SDCRYPT_SECTOR;
    while (count) {
        uint32_t n = count < chunk ? count : chunk;
        if (!sdcrypt_encrypt(&cs->crypt, cs->buf, buf, sector, n))
            return SD_BLOCK_DEVICE_ERROR_UNSUPPORTED;
        block_dev_err_t r = write_blocks_fed(c, cs->buf, sector + SDCRYPT_OFFSET, n);
        if (r != SD_BLOCK_DEVICE_ERROR_NONE)
            return r;
        buf += n * SDCRYPT_SECTOR;
        sector += n;
        count -= n;
    }
    return SD_BLOCK_DEVICE_ERROR_NONE;
}

static uint32_t get_num_sectors_layer(sd_card_t *c) {
    uint32_t n = get_num_sectors_cached(c);
    if (card_kind == KIND_ENCRYPTED)
        return n > SDCRYPT_OFFSET ? n - SDCRYPT_OFFSET : 0;
    return n;
}

static void hex(char *out, const uint8_t *p, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        snprintf(out + 2 * i, 3, "%02x", p[i]);
}

static const char *kind_why;    // why a locked or unsupported card can't be used
// The card is locked because the board had no card key when it was looked at (no owner secret
// yet). That can change without the card changing: mount() looks again once there is one.
static bool locked_without_key;
// The card couldn't be looked at for lack of memory (crypt_alloc): mount() looks again.
static bool locked_no_memory;

// Read the card's sector 0 and backup header (into cs->buf, which isn't in use outside a
// write; the caller has allocated it) and open them with this board's card key (sdcrypt_card_open has the rules). The card must be
// initialized. Fills `info` on SDCRYPT_OK; *from_backup says the backup opened it.
static int open_card(sdcrypt_info_t *info, bool *from_backup) {
    uint8_t *primary = cs->buf, *backup = cs->buf + SDCRYPT_SECTOR;
    if (read_blocks_logged(&card, primary, 0, 1) != SD_BLOCK_DEVICE_ERROR_NONE) {
        memset(info, 0, sizeof(*info));
        return SDCRYPT_PLAIN;   // unreadable: FatFs will say so
    }
    bool have_backup = read_blocks_logged(&card, backup, SDCRYPT_BACKUP, 1) ==
                       SD_BLOCK_DEVICE_ERROR_NONE;
    uint8_t key[CARDKEY_LEN];
    bool have_key = cardkey_get(key);
    int r = sdcrypt_card_open(primary, have_backup ? backup : NULL, have_key ? key : NULL, info,
                              from_backup);
    sdcrypt_wipe(key, sizeof(key));
    return r;
}

// After a mount through the layer succeeded on a card opened from its backup header: the volume
// is really there, so sector 0 can be rewritten from the backup. (Not at detection: a card this
// device once encrypted and then reused elsewhere can keep an old backup at 4096, and its
// sector 0 must not be overwritten on that evidence alone.)
static void repair_after_mount(void) {
    if (!repair_header)
        return;
    repair_header = false;
    // Only after a mount through the layer, so the card is encrypted and cs is there.
    bool fixed = read_blocks_logged(&card, cs->buf, SDCRYPT_BACKUP, 1) == SD_BLOCK_DEVICE_ERROR_NONE &&
                 sdcrypt_is_header(cs->buf) &&
                 write_blocks_fed(&card, cs->buf, 0, 1) == SD_BLOCK_DEVICE_ERROR_NONE;
    log_msg(SOS_LOG_DAEMON | SOS_LOG_WARNING,
            "sd: the card's header was damaged; %s from its backup copy",
            fixed ? "rewrote it" : "using it (rewriting sector 0 failed)");
}

// Find out what kind of card this is, and turn the layer on for an encrypted one. Runs after
// every card initialization (init_layer), and at mount if the kind isn't known. A card that
// can't be used is logged once here; mount() tells the shell each time.
static void detect(void) {
    crypt_off();
    kind_why = NULL;
    repair_header = false;
    locked_without_key = false;
    locked_no_memory = false;
    // Logged when it starts, not at every try: a mount is tried again while memory is short
    // (by hand, or by anything that remounts on a timer).
    static bool no_memory_logged;
    if (!crypt_alloc()) {
        // Not known to be plain, so not usable as one: an encrypted card read without the
        // layer looks like a card with no filesystem.
        card_kind = KIND_LOCKED;
        locked_no_memory = true;
        kind_why = "not enough memory to look at the card (7.5 KB): free some and try again";
        if (!no_memory_logged)
            log_msg(SOS_LOG_DAEMON | SOS_LOG_WARNING, "sd: %s", kind_why);
        no_memory_logged = true;
        return;
    }
    no_memory_logged = false;
    sdcrypt_info_t info;
    bool from_backup;
    int r = open_card(&info, &from_backup);
    if (r == SDCRYPT_PLAIN) {
        card_kind = KIND_PLAIN;
    } else if (r == SDCRYPT_OK && sdcrypt_start(&cs->crypt, info.data_key)) {
        memcpy(card_id, info.card_id, sizeof(card_id));
        card_kind = KIND_ENCRYPTED;
        repair_header = from_backup;
    } else if (r == SDCRYPT_UNSUPPORTED) {
        card_kind = KIND_UNSUPPORTED;
        kind_why = "the card is encrypted in a format this firmware doesn't know";
    } else {
        card_kind = KIND_LOCKED;
        locked_without_key = r == SDCRYPT_NO_DEVKEY;
        kind_why = r == SDCRYPT_OLD_WRAP
            ? "the card was encrypted by older firmware, under a key this firmware doesn't use: "
              "reformat it (`sd format`), or read it on a PC with the key shown at its format"
            : r == SDCRYPT_NO_DEVKEY
            ? (cardkey_possible()
                   ? "the card is encrypted, and this board has no owner secret to open cards "
                     "with yet (it has not been set up)"
                   : "the card is encrypted, and this board has no key for cards (see `rootsecret`)")
            : "the card is encrypted under another board's key, or this board's configuration "
              "was erased since it was formatted (or both header copies are damaged)";
    }
    sdcrypt_wipe(&info, sizeof(info));
    if (card_kind != KIND_ENCRYPTED)
        crypt_free();                       // only an open encrypted card keeps the memory
    if (kind_why)
        log_msg(SOS_LOG_DAEMON | SOS_LOG_WARNING, "sd: %s", kind_why);
}

// Every card initialization finds the card's kind again, so the layer always matches the card
// that's in, however it was reinitialized (a mount, or the driver after an error).
static DSTATUS (*lib_init)(sd_card_t *);

static DSTATUS init_layer(sd_card_t *c) {
    // The library calls init on every disk_initialize(), also when the card is already up:
    // detect only on a real initialization (or if the kind was forgotten), so a locked card is
    // logged once per insertion rather than at every try.
    bool was_up = !(c->state.m_Status & STA_NOINIT);
    DSTATUS st = lib_init(c);
    if (st & STA_NOINIT)
        crypt_off();
    else if (!was_up || card_kind == KIND_UNKNOWN)
        detect();
    return st;
}

static bool driver_init(void) {
    static bool ready;
    if (!ready && sd_init_driver()) {
        lib_write_blocks = card.write_blocks;   // set by sd_init_driver()
        card.write_blocks = write_blocks_layer;
        lib_read_blocks = card.read_blocks;
        card.read_blocks = read_blocks_layer;
        lib_get_num_sectors = card.get_num_sectors;
        card.get_num_sectors = get_num_sectors_layer;
        lib_init = card.init;
        card.init = init_layer;
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

static uint32_t mount_generation;   // counts mounts, for sdcard_generation()

static bool mount(shell_t *sh) {
    if (card.state.mounted)
        return true;
    uint64_t t0 = time_us_64();
    if (!(disk_initialize(0) & STA_NOINIT)) {    // no card: f_mount says so below
        if (card_kind == KIND_LOCKED && locked_without_key) {
            // The board has been set up since (it has an owner secret now): the answer, or at
            // least the reason, may be different.
            uint8_t key[CARDKEY_LEN];
            if (cardkey_get(key))
                card_kind = KIND_UNKNOWN;
            sdcrypt_wipe(key, sizeof(key));
        }
        if (card_kind == KIND_LOCKED && locked_no_memory)
            card_kind = KIND_UNKNOWN;           // there may be memory now
        if (card_kind == KIND_UNKNOWN)          // initialized earlier, kind forgotten since
            detect();
        if (card_kind == KIND_LOCKED || card_kind == KIND_UNSUPPORTED) {
            if (sh && locked_no_memory)
                shell_printf(sh, "sd: %s\n", kind_why);
            else if (sh)
                shell_printf(sh, "sd: %s; `sd format` erases it for use here\n", kind_why);
            return false;
        }
    }
    FRESULT fr = f_mount(&card.state.fatfs, sd_get_drive_prefix(&card), 1);
    uint32_t ms = (uint32_t)((time_us_64() - t0) / 1000);
    if (fr != FR_OK) {
        if (sh)
            shell_printf(sh, "mount failed: %s (%d) after %lu ms\n", FRESULT_str(fr), fr,
                         (unsigned long)ms);
        else if (!quiet)
            log_msg(SOS_LOG_DAEMON | SOS_LOG_WARNING,
                    "sd: mount failed: %s (%d)", FRESULT_str(fr), fr);
        return false;
    }
    card.state.mounted = true;
    mount_generation++;
    if (card_kind == KIND_ENCRYPTED)
        repair_after_mount();
    log_msg(SOS_LOG_DAEMON | SOS_LOG_NOTICE,
            "sd: mounted %s%s in %lu ms", fs_type(card.state.fatfs.fs_type),
            card_kind == KIND_ENCRYPTED ? " (encrypted)" : "", (unsigned long)ms);
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
    if (card_kind == KIND_ENCRYPTED) {
        char id[2 * SDCRYPT_ID_LEN + 1];
        hex(id, card_id, SDCRYPT_ID_LEN);
        shell_printf(sh, "crypt:  XTS-AES-128, card id %s\n", id);
    } else {
        shell_printf(sh, "crypt:  none (a plain card)\n");
    }
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

#define TEST_BLOCK 4096                 // written and read back at a time

// The test itself, with `buf` (TEST_BLOCK bytes) from cmd_test().
static void test_run(shell_t *sh, int blocks, const char *keep, uint8_t *buf) {
    const uint32_t lines_per_block = TEST_BLOCK / TEST_LINE;
    const char *name = "/sos-test.txt";
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
        fr = f_write(&f, buf, TEST_BLOCK, &bw);
        if (time_us_64() - b0 > worst)
            worst = time_us_64() - b0;
        if (fr == FR_OK && bw != TEST_BLOCK)
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
        ok = f_read(&f, buf, TEST_BLOCK, &br) == FR_OK && br == TEST_BLOCK;
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
    unsigned kb = (unsigned)(blocks * TEST_BLOCK / 1024);
    shell_printf(sh, "write %u KB: %llu ms (%llu KB/s), slowest 4 KB write %llu ms\n", kb,
                 wus / 1000, kb * 1000000ull / wus, worst / 1000);
    shell_printf(sh, "read  %u KB: %llu ms (%llu KB/s), data %s\n", kb, rus / 1000,
                 kb * 1000000ull / rus, ok ? "verified" : "MISMATCH");
}

static void cmd_test(shell_t *sh, const char *size_kb, const char *keep) {
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
    if (!mount(sh))
        return;
    // The block is only needed while the test runs, so it isn't kept as a static buffer.
    uint8_t *buf = heap_alloc(HEAP_FILES, TEST_BLOCK);
    if (!buf) {
        shell_printf(sh, "sd test: out of memory\n");
        return;
    }
    test_run(sh, blocks, keep, buf);
    heap_release(HEAP_FILES, buf);
}

// Formatting, unmounting or reinitializing the card would break file commands with open files.
static bool files_in_use(shell_t *sh) {
    if (!files_busy())
        return false;
    shell_printf(sh, "files are open (a cat, cp or ls running, or an SFTP transfer); wait for it to end\n");
    return true;
}

// Set by `sd unmount` (the card is about to come out), cleared by the next mount asked for.
static bool unmounted_by_hand;

bool sdcard_mount_retry(void) {
    if (unmounted_by_hand || card.state.mounted)
        return card.state.mounted;
    quiet = true;
    bool ok = driver_init() && mount(NULL);
    quiet = false;
    return ok;
}

bool sdcard_mounted(void) {
    return card.state.mounted;
}

uint32_t sdcard_generation(void) {
    return mount_generation;
}

bool sdcard_mount(shell_t *sh) {
    unmounted_by_hand = false;
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
    crypt_off();
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
static bool format_plain;                   // no encryption

// Start an encrypted card: a new random data key and card id in a header at sector 0 and its
// backup, then turn the layer on, so f_mkfs writes the volume encrypted.
// The data key of a card being formatted, from start_encrypted() until show_new_key() shows and
// wipes it.
static uint8_t new_key[SDCRYPT_KEY_LEN];

static bool start_encrypted(shell_t *sh) {
    uint8_t id[SDCRYPT_ID_LEN], nonce[SDCRYPT_NONCE_LEN], key[SDCRYPT_KEY_LEN];
    uint8_t wrap[CARDKEY_LEN];
    // The first encrypted card on a board that was never set up makes the owner secret (and
    // saves the configuration to hold it).
    if (!cardkey_ensure() || !cardkey_get(wrap)) {
        shell_printf(sh, "format failed: no key to encrypt the card with (see dmesg)\n");
        return false;
    }
    WC_RNG rng;
    bool ok = wc_InitRng(&rng) == 0;
    if (ok) {
        ok = wc_RNG_GenerateBlock(&rng, id, sizeof(id)) == 0 &&
             wc_RNG_GenerateBlock(&rng, nonce, sizeof(nonce)) == 0 &&
             wc_RNG_GenerateBlock(&rng, key, sizeof(key)) == 0;
        wc_FreeRng(&rng);
    }
    // The layer is off while formatting (crypt_off() in on_format_confirm): get its memory.
    if (ok && !crypt_alloc()) {
        sdcrypt_wipe(key, sizeof(key));
        sdcrypt_wipe(wrap, sizeof(wrap));
        shell_printf(sh, "format failed: out of memory\n");
        return false;
    }
    uint8_t *header = ok ? cs->buf : NULL;
    ok = ok && sdcrypt_header_build(header, wrap, id, nonce, key) &&
         write_blocks_fed(&card, header, 0, 1) == SD_BLOCK_DEVICE_ERROR_NONE &&
         write_blocks_fed(&card, header, SDCRYPT_BACKUP, 1) == SD_BLOCK_DEVICE_ERROR_NONE &&
         sdcrypt_start(&cs->crypt, key);
    if (ok)
        memcpy(new_key, key, sizeof(new_key));
    sdcrypt_wipe(key, sizeof(key));
    sdcrypt_wipe(wrap, sizeof(wrap));
    if (!ok) {
        shell_printf(sh, "format failed: couldn't write the encryption header\n");
        return false;
    }
    memcpy(card_id, id, sizeof(card_id));
    card_kind = KIND_ENCRYPTED;
    return true;
}

// A plain card: no layer, and no header copy left behind (f_mkfs overwrites sector 0, not the
// backup at 4096, which would otherwise make the card look encrypted again).
static bool start_plain(shell_t *sh) {
    // The card was looked at before the format, which turns the layer on for an encrypted
    // one: turn it off and forget that card's key now (KIND_ENCRYPTED must not outlive it).
    crypt_off();
    uint8_t zero[SDCRYPT_SECTOR] = {0};
    if (write_blocks_fed(&card, zero, SDCRYPT_BACKUP, 1) != SD_BLOCK_DEVICE_ERROR_NONE) {
        shell_printf(sh, "format failed: couldn't write the card\n");
        return false;
    }
    card_kind = KIND_PLAIN;
    return true;
}

// After an encrypted format, the one time the card's data key is shown: it's what a PC needs to
// read the card (or a replacement board, later) if this device is lost. Formatting is console
// only, so the key never goes to an SSH session. Shown once the header and file system are
// written, whether or not the mount that follows works, so a formatted card always had its key
// shown.
static void show_new_key(shell_t *sh) {
    char line[16 + 2 * SDCRYPT_KEY_LEN];
    shell_printf(sh, "\nStore this key for recovery. It's shown only now, and it's the only\n");
    shell_printf(sh, "way to read the card if this device is lost. Anyone with it can read\n");
    shell_printf(sh, "the card: keep it offline. On a PC: tools/host/sdcrypt.c, or cryptsetup.\n");
    hex(line, card_id, SDCRYPT_ID_LEN);
    shell_printf(sh, "card id:  %s\n", line);
    // The key is formatted here, not by shell_printf, so no copy is left in its stack buffer.
    memcpy(line, "data key: ", 10);
    hex(line + 10, new_key, SDCRYPT_KEY_LEN);
    size_t n = strlen(line);
    line[n++] = '\n';
    shell_write(sh, line, n);
    sdcrypt_wipe(line, sizeof(line));
    sdcrypt_wipe(new_key, sizeof(new_key));
    log_msg(SOS_LOG_DAEMON | SOS_LOG_NOTICE, "sd: the new card's key was shown on the console");
}

static void on_format_confirm(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (!answer || strcmp(answer, "yes")) {
        shell_printf(sh, "not formatted\n");
        return;
    }
    if (files_in_use(sh))
        return;
    void *work = heap_alloc(HEAP_FILES, FORMAT_WORK_BYTES);
    if (!work) {
        shell_printf(sh, "not formatted: out of memory\n");
        return;
    }
    const char *drive = sd_get_drive_prefix(&card);
    f_unmount(drive);
    card.state.mounted = false;
    crypt_off();
    const char *name = format_type == FM_EXFAT ? "exFAT" : "FAT32";
    if ((disk_initialize(0) & STA_NOINIT) ||
        !(format_plain ? start_plain(sh) : start_encrypted(sh))) {
        if (disk_status(0) & STA_NOINIT)
            shell_printf(sh, "format failed: no card\n");
        crypt_off();
        heap_release(HEAP_FILES, work);
        return;
    }
    log_msg(SOS_LOG_DAEMON | SOS_LOG_NOTICE, "sd: formatting the card as %s%s", name,
            format_plain ? "" : ", encrypted");

    MKFS_PARM opt = { .fmt = format_type, .n_fat = 1, .align = FORMAT_ALIGN_SECTORS };
    uint64_t t0 = time_us_64();
    memset(&io_stats, 0, sizeof(io_stats));
    formatting = true;
    // No FM_SFD: writes a new partition table with one partition over the whole card.
    FRESULT fr = f_mkfs(drive, &opt, work, FORMAT_WORK_BYTES);
    formatting = false;
    uint32_t ms = (uint32_t)((time_us_64() - t0) / 1000);
    heap_release(HEAP_FILES, work);
    log_msg(SOS_LOG_DAEMON | SOS_LOG_INFO,
            "sd: format did %lu reads, %lu writes (%lu sectors), last write %lu+%lu",
            (unsigned long)io_stats.reads, (unsigned long)io_stats.writes,
            (unsigned long)io_stats.sectors, (unsigned long)io_stats.last_sector,
            (unsigned long)io_stats.last_count);
    crypt_off();                            // mount() finds the card's kind again
    if (fr != FR_OK) {
        sdcrypt_wipe(new_key, sizeof(new_key));
        log_msg(SOS_LOG_DAEMON | SOS_LOG_ERR,
                "sd: format failed: %s (%d) after %lu ms", FRESULT_str(fr), fr,
                (unsigned long)ms);
        shell_printf(sh, "format failed: %s\n", FRESULT_str(fr));
        return;
    }
    log_msg(SOS_LOG_DAEMON | SOS_LOG_NOTICE,
            "sd: formatted as %s in %lu ms", name, (unsigned long)ms);
    if (mount(sh)) {
        shell_printf(sh, "formatted in %lu.%lu s\n", (unsigned long)(ms / 1000),
                     (unsigned long)(ms % 1000 / 100));
        cmd_info(sh);
    }
    if (!format_plain)
        show_new_key(sh);
}

static void cmd_format(shell_t *sh, const char *type, const char *mode) {
    if (!sh->io->admin) {
        shell_printf(sh, "the card can only be formatted on the serial console\n");
        return;
    }
    if (files_in_use(sh))
        return;
    if (type && !strcmp(type, "plain") && !mode) {      // `sd format plain`
        mode = type;
        type = NULL;
    }
    format_plain = mode && !strcmp(mode, "plain");
    if (!type || !strcmp(type, "exfat"))
        format_type = FM_EXFAT;
    else if (!strcmp(type, "fat32"))
        format_type = FM_FAT32;
    else
        type = "";
    if ((type && *type == '\0') || (mode && !format_plain)) {
        shell_printf(sh, "usage: sd format [exfat|fat32] [plain]\n");
        return;
    }
    if (!format_plain && !cardkey_possible()) {
        shell_printf(sh, "this board's root secret can't be used (see `rootsecret`), so the card "
                         "can't be encrypted: use `sd format plain`\n");
        return;
    }

    // Make sure there's a card, and show what's on it now.
    if (card.state.mounted || mount(sh))
        cmd_info(sh);
    else if (!card.state.sectors) {
        shell_printf(sh, "no card\n");
        return;
    }
    // One line per shell_printf (its buffer is 256 bytes). One-line prompt: log lines redraw
    // only the input line.
    const char *name = format_type == FM_EXFAT ? "exFAT" : "FAT32";
    if (format_plain) {
        shell_printf(sh, "This erases the whole card (all partitions) and formats it as %s, not\n",
                     name);
        shell_printf(sh, "encrypted: anyone with the card can read it.\n");
    } else {
        shell_printf(sh, "This erases the whole card (all partitions) and formats it as %s,\n",
                     name);
        shell_printf(sh, "encrypted with a new key that this device keeps. A PC can't read it.\n");
        shell_printf(sh, "The card's key is shown once, when the format finishes: store it\n");
        shell_printf(sh, "offline for recovery. It's the only way to read the card after this\n");
        shell_printf(sh, "board's configuration is erased (`config reset`) or provisioned again:\n");
        shell_printf(sh, "either locks the cards the board encrypted before.\n");
        shell_printf(sh, "Files from before stay on the card unencrypted\n");
        shell_printf(sh, "until new data overwrites them: for a card that held private files\n");
        shell_printf(sh, "unencrypted, use a new card instead.\n");
    }
    shell_printf(sh, "SSH sessions pause until it's done.\n");
    shell_ask(sh, "Erase the card? Type 'yes': ", false, on_format_confirm, NULL);
}

// --- owner change -------------------------------------------------------------------------------

// The configuration was erased (`config reset`): the owner secret is gone, so an encrypted
// card must not stay open under the old one. Unmounts it and forgets its key.
void sdcard_owner_gone(void) {
    if (card_kind != KIND_ENCRYPTED && card_kind != KIND_LOCKED)
        return;
    if (card.state.mounted)
        f_unmount(sd_get_drive_prefix(&card));
    card.state.mounted = false;
    crypt_off();
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
        cmd_format(sh, arg, arg2);
    else if (!strcmp(sub, "unmount")) {
        if (files_in_use(sh))
            return;
        // It's for taking the card out, which needs someone at the device anyway; a session
        // could otherwise unmount it under everyone else.
        if (!sh->io->admin) {
            shell_printf(sh, "the card can only be unmounted on the serial console\n");
            return;
        }
        f_unmount(sd_get_drive_prefix(&card));
        card.state.mounted = false;
        unmounted_by_hand = true;           // no quiet remount (sdcard_mount_retry) under the hand
        crypt_off();                        // forget the card's key
        card.state.m_Status |= STA_NOINIT;  // a swapped card is initialized afresh
        shell_printf(sh, "unmounted; safe to remove the card\n");
    } else
        shell_printf(sh, "usage: sd [info|test [KB [keep]]|speed [MHz]|format [exfat|fat32] [plain]|"
                         "unmount]\n");
}
