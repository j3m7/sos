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

#include "core/devkey.h"
#include "core/log.h"
#include "core/wdog.h"
#include "files/files.h"
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

// --- encryption (see files/sdcrypt.h) ----------------------------------------------------------
//
// Between FatFs and the driver: on an encrypted card, FatFs's sector n is the card's sector
// n + SDCRYPT_OFFSET, decrypted after reading and encrypted before writing. The kind of card is
// found at each mount (detect()).

static enum {
    KIND_UNKNOWN,           // not mounted since the card was (re)initialized
    KIND_PLAIN,
    KIND_ENCRYPTED,         // the layer is on
    KIND_LOCKED,            // encrypted, but the device key doesn't open it (or there is none)
    KIND_UNSUPPORTED,       // encrypted in a format this firmware doesn't know
} card_kind;
static sdcrypt_t crypt;
static uint8_t card_id[SDCRYPT_ID_LEN];
static uint8_t crypt_buf[8 * SDCRYPT_SECTOR];     // sectors encrypted before writing
static uint8_t header_buf[SDCRYPT_SECTOR];

static void crypt_off(void) {
    sdcrypt_stop(&crypt);
    card_kind = KIND_UNKNOWN;
}

static block_dev_err_t read_blocks_layer(sd_card_t *c, uint8_t *buf, uint32_t sector,
                                         uint32_t count) {
    if (card_kind == KIND_LOCKED || card_kind == KIND_UNSUPPORTED)
        return SD_BLOCK_DEVICE_ERROR_NO_DEVICE;
    if (card_kind != KIND_ENCRYPTED)
        return read_blocks_logged(c, buf, sector, count);
    block_dev_err_t r = read_blocks_logged(c, buf, sector + SDCRYPT_OFFSET, count);
    if (r == SD_BLOCK_DEVICE_ERROR_NONE && !sdcrypt_decrypt(&crypt, buf, sector, count))
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
    // crypt_buf, a chunk at a time.
    const uint32_t chunk = sizeof(crypt_buf) / SDCRYPT_SECTOR;
    while (count) {
        uint32_t n = count < chunk ? count : chunk;
        if (!sdcrypt_encrypt(&crypt, crypt_buf, buf, sector, n))
            return SD_BLOCK_DEVICE_ERROR_UNSUPPORTED;
        block_dev_err_t r = write_blocks_fed(c, crypt_buf, sector + SDCRYPT_OFFSET, n);
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

// Read the header into header_buf: physical sector 0, or its backup copy if sector 0 is neither
// a header nor a plain card's boot sector. False if there's no header (a plain card) or the card
// can't be read.
static bool read_header(bool *backup) {
    *backup = false;
    if (read_blocks_logged(&card, header_buf, 0, 1) != SD_BLOCK_DEVICE_ERROR_NONE)
        return false;
    if (sdcrypt_is_header(header_buf))
        return true;
    if (header_buf[510] == 0x55 && header_buf[511] == 0xaa)     // MBR or FAT boot sector
        return false;
    if (read_blocks_logged(&card, header_buf, SDCRYPT_BACKUP, 1) != SD_BLOCK_DEVICE_ERROR_NONE ||
        !sdcrypt_is_header(header_buf))
        return false;
    *backup = true;
    return true;
}

static void hex(char *out, const uint8_t *p, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        snprintf(out + 2 * i, 3, "%02x", p[i]);
}

// Find out what kind of card this is, and turn the layer on for an encrypted one. The card must
// be initialized. Says why a card can't be used, to `sh` or in the log.
static void detect(shell_t *sh) {
    crypt_off();
    bool backup;
    if (!read_header(&backup)) {
        card_kind = KIND_PLAIN;
        return;
    }
    if (backup)
        log_msg(SOS_LOG_WARNING, "sd: the card's header is damaged; using its backup copy");
    const char *why = NULL;
    sdcrypt_info_t info;
    int r = devkey() ? sdcrypt_header_open(header_buf, devkey(), &info) : SDCRYPT_WRONG_KEY;
    if (r == SDCRYPT_OK && sdcrypt_start(&crypt, info.data_key)) {
        memcpy(card_id, info.card_id, sizeof(card_id));
        card_kind = KIND_ENCRYPTED;
    } else if (r == SDCRYPT_UNSUPPORTED) {
        card_kind = KIND_UNSUPPORTED;
        why = "the card is encrypted in a format this firmware doesn't know";
    } else {
        card_kind = KIND_LOCKED;
        why = devkey() ? "the card is encrypted with another device's key (or its header is damaged)"
                       : "the card is encrypted, but this device has no device key";
    }
    sdcrypt_wipe(&info, sizeof(info));
    if (why) {
        if (sh)
            shell_printf(sh, "sd: %s; `sd format` erases it for use here\n", why);
        else
            log_msg(SOS_LOG_WARNING, "sd: %s", why);
    }
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
    if (!(disk_initialize(0) & STA_NOINIT)) {    // no card: f_mount says so below
        detect(sh);
        if (card_kind == KIND_LOCKED || card_kind == KIND_UNSUPPORTED)
            return false;
    }
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
    log_msg(SOS_LOG_NOTICE, "sd: mounted %s%s in %lu ms", fs_type(card.state.fatfs.fs_type),
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
static bool start_encrypted(shell_t *sh) {
    uint8_t id[SDCRYPT_ID_LEN], nonce[SDCRYPT_NONCE_LEN], key[SDCRYPT_KEY_LEN];
    WC_RNG rng;
    bool ok = wc_InitRng(&rng) == 0;
    if (ok) {
        ok = wc_RNG_GenerateBlock(&rng, id, sizeof(id)) == 0 &&
             wc_RNG_GenerateBlock(&rng, nonce, sizeof(nonce)) == 0 &&
             wc_RNG_GenerateBlock(&rng, key, sizeof(key)) == 0;
        wc_FreeRng(&rng);
    }
    ok = ok && sdcrypt_header_build(header_buf, devkey(), id, nonce, key) &&
         write_blocks_fed(&card, header_buf, 0, 1) == SD_BLOCK_DEVICE_ERROR_NONE &&
         write_blocks_fed(&card, header_buf, SDCRYPT_BACKUP, 1) == SD_BLOCK_DEVICE_ERROR_NONE &&
         sdcrypt_start(&crypt, key);
    sdcrypt_wipe(key, sizeof(key));
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
    memset(header_buf, 0, sizeof(header_buf));
    if (write_blocks_fed(&card, header_buf, SDCRYPT_BACKUP, 1) != SD_BLOCK_DEVICE_ERROR_NONE) {
        shell_printf(sh, "format failed: couldn't write the card\n");
        return false;
    }
    card_kind = KIND_PLAIN;
    return true;
}

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
    crypt_off();
    const char *name = format_type == FM_EXFAT ? "exFAT" : "FAT32";
    if ((disk_initialize(0) & STA_NOINIT) ||
        !(format_plain ? start_plain(sh) : start_encrypted(sh))) {
        if (disk_status(0) & STA_NOINIT)
            shell_printf(sh, "format failed: no card\n");
        crypt_off();
        free(work);
        return;
    }
    log_msg(SOS_LOG_NOTICE, "sd: formatting the card as %s%s", name,
            format_plain ? "" : ", encrypted");

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
    crypt_off();                            // mount() finds the card's kind again
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
    if (!format_plain && !devkey()) {
        shell_printf(sh, "this device has no device key, so it can't encrypt: use `sd format plain`\n");
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
    if (format_plain)
        shell_printf(sh, "This erases the whole card (all partitions) and formats it as %s, not\n"
                     "encrypted: anyone with the card can read it.\n",
                     format_type == FM_EXFAT ? "exFAT" : "FAT32");
    else
        shell_printf(sh, "This erases the whole card (all partitions) and formats it as %s,\n"
                     "encrypted with a new key that this device keeps. A PC can't read it;\n"
                     "`sd key` shows the key, to keep somewhere safe for recovery.\n",
                     format_type == FM_EXFAT ? "exFAT" : "FAT32");
    shell_printf(sh, "SSH sessions pause until it's done.\n");
    shell_ask(sh, "Erase the card? Type 'yes': ", false, on_format_confirm, NULL);
}

// --- key ---------------------------------------------------------------------------------------

// Show the card's data key, to keep offline: it's what a PC needs to read the card (or a
// replacement board, later) if this device is lost.
static void cmd_key(shell_t *sh) {
    if (!sh->io->admin) {
        shell_printf(sh, "the card's key can only be shown on the serial console\n");
        return;
    }
    if (!mount(sh))
        return;
    if (card_kind != KIND_ENCRYPTED) {
        shell_printf(sh, "the card isn't encrypted\n");
        return;
    }
    bool backup;
    sdcrypt_info_t info;
    if (!read_header(&backup) || sdcrypt_header_open(header_buf, devkey(), &info) != SDCRYPT_OK) {
        shell_printf(sh, "can't read the card's header\n");
        return;
    }
    char id[2 * SDCRYPT_ID_LEN + 1], key[2 * SDCRYPT_KEY_LEN + 1];
    hex(id, info.card_id, SDCRYPT_ID_LEN);
    hex(key, info.data_key, SDCRYPT_KEY_LEN);
    sdcrypt_wipe(&info, sizeof(info));
    shell_printf(sh, "card id:  %s\n", id);
    shell_printf(sh, "data key: %s\n", key);
    sdcrypt_wipe(key, sizeof(key));
    shell_printf(sh, "Anyone with this key can read the card: keep it offline. To read the card on\n"
                     "a PC: tools/host/sdcrypt.c, or cryptsetup (`sdcrypt info` prints the line).\n");
    log_msg(SOS_LOG_NOTICE, "sd: the card's key was shown on the console");
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
    else if (!strcmp(sub, "key"))
        cmd_key(sh);
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
        crypt_off();                        // forget the card's key
        card.state.m_Status |= STA_NOINIT;  // a swapped card is initialized afresh
        shell_printf(sh, "unmounted; safe to remove the card\n");
    } else
        shell_printf(sh, "usage: sd [info|test [KB [keep]]|speed [MHz]|format [exfat|fat32] [plain]|key|unmount]\n");
}
