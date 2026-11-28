// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The checks of stored state on the board (core/statecheck.h, docs/design/health.md §9.3): each
// step gathers what its check reads and hands it to statecheck.c's judging. They read only:
// nothing here writes flash, OTP or the card, or mounts a card.
//
// - config: each slot through settings.c's store (the seal key it holds for saves; nothing is
//   derived again), one slot per step: an AES-GCM open of 2.2 KB and a CRC, a few milliseconds.
// - rootsecret: what rootsource_init() recorded at boot, the soft-lock register of the
//   secret's OTP page, and on an open board the in-place check of the flash record that
//   `rootsecret` runs. No OTP row is read.
// - otp: the named rows of statecheck.h only (CRIT1, BOOT_FLAGS1, the boot key slots), through
//   otpread.h's unguarded aliases, which can't write and don't fault; never the root secret's
//   pages. The running image's key hashed as `bootkey` does.
// - failcount: the sector in place.
// - hostkey: hostkey.c's digest of the stored page against the page now, never opened.
// - card: yields to whatever owns the card, never mounts one; hashes /certs/roots.pem 2 KB a
//   step, holding the files layer (files_hold) in between so a format or unmount waits.

#include "core/statecheck.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/regs/otp_data.h"
#include "pico/stdlib.h"

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/hash.h>
#include <wolfssl/wolfcrypt/sha256.h>

#include "core/consoleonly.h"
#include "core/failflash.h"
#include "core/heaptag.h"
#include "core/logfile.h"
#include "core/otpread.h"
#include "core/rootflash.h"
#include "core/rootsource.h"
#include "core/settings.h"
#include "files/files.h"
#include "files/sdcard.h"
#include "ssh/hostkey.h"
#include "ssh/sshd.h"
#include "tls/certstore.h"

_Static_assert(STATECHECK_CRIT1_ROW == OTP_DATA_CRIT1_ROW &&
               STATECHECK_CRIT1_ROW + STATECHECK_CRIT1_COPIES - 1 == OTP_DATA_CRIT1_R7_ROW &&
               STATECHECK_FLAGS1_ROW == OTP_DATA_BOOT_FLAGS1_ROW &&
               STATECHECK_FLAGS1_ROW + STATECHECK_FLAGS1_COPIES - 1 == OTP_DATA_BOOT_FLAGS1_R2_ROW &&
               STATECHECK_BOOTKEY_ROW == OTP_DATA_BOOTKEY0_0_ROW &&
               STATECHECK_BOOTKEY_ROW + STATECHECK_BOOTKEY_ROWS - 1 == OTP_DATA_BOOTKEY3_15_ROW,
               "statecheck.h names the SDK's rows");
_Static_assert(STATECHECK_BOOTKEY_ROW + STATECHECK_BOOTKEY_ROWS <= ROOTOTP_ROW(0, 0),
               "the otp check stays below the root secret's pages");
_Static_assert(STATECHECK_SECURE_BOOT == OTP_DATA_CRIT1_SECURE_BOOT_ENABLE_BITS &&
               STATECHECK_SECURE_DEBUG_DISABLE == OTP_DATA_CRIT1_SECURE_DEBUG_DISABLE_BITS &&
               STATECHECK_DEBUG_DISABLE == OTP_DATA_CRIT1_DEBUG_DISABLE_BITS &&
               STATECHECK_CRIT1_FLAGS == (OTP_DATA_CRIT1_SECURE_BOOT_ENABLE_BITS |
                                          OTP_DATA_CRIT1_SECURE_DEBUG_DISABLE_BITS |
                                          OTP_DATA_CRIT1_DEBUG_DISABLE_BITS |
                                          OTP_DATA_CRIT1_BOOT_ARCH_BITS |
                                          OTP_DATA_CRIT1_GLITCH_DETECTOR_ENABLE_BITS |
                                          OTP_DATA_CRIT1_GLITCH_DETECTOR_SENS_BITS) &&
               STATECHECK_KEY_VALID == OTP_DATA_BOOT_FLAGS1_KEY_VALID_BITS &&
               STATECHECK_KEY_INVALID == OTP_DATA_BOOT_FLAGS1_KEY_INVALID_BITS,
               "statecheck.h names the SDK's bits");

// --- config ----------------------------------------------------------------------------------

// The slot read by the first step, kept for the second. One check runs at a time (health.c), so
// one copy does; *state points at it while the check is part-way.
static statecheck_config_t config_seen;

health_step_t statecheck_config_step(void **state, bool cancel, health_result_t *out) {
    if (cancel) {
        *state = NULL;
        return HEALTH_DONE;
    }
    if (!*state) {
        memset(&config_seen, 0, sizeof(config_seen));
        config_seen.persistent = settings_persistent();
        if (!config_seen.persistent) {
            statecheck_judge_config(&config_seen, out);
            return HEALTH_DONE;
        }
        settings_config_slot(0, &config_seen.slot[0]);
        *state = &config_seen;
        return HEALTH_MORE;
    }
    settings_config_slot(1, &config_seen.slot[1]);
    *state = NULL;
    statecheck_judge_config(&config_seen, out);
    return HEALTH_DONE;
}

// --- rootsecret ------------------------------------------------------------------------------

static void root_gather(statecheck_root_t *r) {
    memset(r, 0, sizeof(*r));
    r->source = rootsource();
    r->found = rootsource_otp_found();
    r->page = ROOTOTP_FIRST_PAGE + rootsource_otp_index();
    rootsource_otp_detail(&r->secret_ok, &r->locks_ok, &r->open_page);
    r->soft_locked = rootsource_otp_soft_locked();
    r->console_only = consoleonly();
    if (r->source == ROOTSRC_FLASH) {
        r->flash_usable = rootflash_usable();
        r->flash_state = rootflash_state();
        r->flash_degraded = rootflash_degraded();
    }
}

health_step_t statecheck_root_step(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (!cancel) {
        statecheck_root_t r;
        root_gather(&r);
        statecheck_judge_root(&r, out);
    }
    return HEALTH_DONE;
}

// --- otp -------------------------------------------------------------------------------------

// The running image's boot key hash, as `bootkey` finds it: the signature block is added after
// linking, just past what the linker calls the end.
static bool image_key_hash(uint8_t hash[BOOTKEY_HASH_LEN]) {
    extern char __flash_binary_start, __flash_binary_end;
    const uint8_t *image = (const uint8_t *)&__flash_binary_start;
    uintptr_t start = (uintptr_t)&__flash_binary_start, end = (uintptr_t)&__flash_binary_end;
    size_t len = (size_t)(end - start) + 4096;
    size_t flash_left = XIP_BASE + PICO_FLASH_SIZE_BYTES - (uintptr_t)image;
    if (len > flash_left)
        len = flash_left;
    const uint8_t *pubkey;
    return bootkey_image_pubkey(image, len, &pubkey) == BOOTKEY_IMAGE_SIGNED &&
           wc_Sha256Hash(pubkey, BOOTKEY_PUBKEY_LEN, hash) == 0;
}

// Only the rows statecheck.h names (all below 0x0c0), through the read aliases.
static void otp_gather(statecheck_otp_t *o) {
    memset(o, 0, sizeof(*o));
    otpread_status_t st;
    otpread_status(&st);
    o->latched = st.latched;
    for (unsigned i = 0; i < STATECHECK_CRIT1_COPIES; i++)
        o->crit1[i] = otpread_raw(STATECHECK_CRIT1_ROW + i);
    for (unsigned i = 0; i < STATECHECK_FLAGS1_COPIES; i++)
        o->flags1[i] = otpread_raw(STATECHECK_FLAGS1_ROW + i);
    for (unsigned i = 0; i < STATECHECK_BOOTKEY_ROWS; i++) {
        o->key_raw[i] = otpread_raw(STATECHECK_BOOTKEY_ROW + i);
        o->key_ecc[i] = otpread_ecc(STATECHECK_BOOTKEY_ROW + i);
    }
    o->image_signed = image_key_hash(o->image_hash);
#ifdef NDEBUG
    o->release = true;                  // the release and size presets (Release, MinSizeRel)
#endif
}

health_step_t statecheck_otp_step(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (!cancel) {
        static statecheck_otp_t o;      // 400 bytes: kept off the stack
        otp_gather(&o);
        statecheck_judge_otp(&o, out);
    }
    return HEALTH_DONE;
}

// --- failcount -------------------------------------------------------------------------------

static void failcount_gather(statecheck_failcount_t *f) {
    memset(f, 0, sizeof(*f));
    f->usable = failflash_usable();
    failcount_inspect(failflash_sector(), &f->info);
    f->count = failflash_count();
}

health_step_t statecheck_failcount_step(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (!cancel) {
        statecheck_failcount_t f;
        failcount_gather(&f);
        statecheck_judge_failcount(&f, out);
    }
    return HEALTH_DONE;
}

// --- hostkey ---------------------------------------------------------------------------------

static void hostkey_gather(statecheck_hostkey_t *h) {
    hostkey_health_t k;
    hostkey_health(&k);
    h->have_owner = settings_has_owner_secret();
    h->have_key = k.have_key;
    h->gave_up = k.gave_up;
    h->unsealed = k.unsealed;
    h->page_matches = k.page_matches;
    h->erased = k.erased;
    const char *fp = sshd_host_fingerprint();
    h->fingerprint = fp && fp[0] ? fp : NULL;
}

health_step_t statecheck_hostkey_step(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (!cancel) {
        statecheck_hostkey_t h;
        hostkey_gather(&h);
        statecheck_judge_hostkey(&h, out);
    }
    return HEALTH_DONE;
}

// --- card ------------------------------------------------------------------------------------

#define CARD_CHUNK 2048                 // bytes of roots.pem hashed per step

typedef struct {
    statecheck_card_t seen;
    FIL file;
    wc_Sha256 sha;
    uint32_t generation;                // the mount being read
    uint8_t buf[CARD_CHUNK];
} card_state_t;

static bool approved(void) {
    const uint8_t *h = settings()->certs_hash;
    for (unsigned i = 0; i < CONFIG_CERTS_HASH_LEN; i++)
        if (h[i])
            return true;
    return false;
}

static statecheck_path_t path_kind(const char *path) {
    FILINFO fi;
    FRESULT fr = f_stat(path, &fi);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH)
        return STATECHECK_PATH_MISSING;
    if (fr != FR_OK)
        return STATECHECK_PATH_ERROR;
    return fi.fattrib & AM_DIR ? STATECHECK_PATH_DIR : STATECHECK_PATH_FILE;
}

// Whatever owns the card now, or NULL: the card check then leaves it alone.
static const char *card_yield(const sdcard_health_t *sd) {
    if (sd->busy)
        return sd->busy;
    if (sd->mounted && files_busy())
        return "files open (a file command or SFTP); next run";
    return NULL;
}

static void card_free(void **state, bool close) {
    card_state_t *c = *state;
    if (c) {
        if (close) {
            f_close(&c->file);
            files_release();
        }
        wc_Sha256Free(&c->sha);
        heap_release(HEAP_FILES, c);
    }
    *state = NULL;
}

// The first step: who owns the card, the two directories, and the start of the roots file.
static health_step_t card_start(void **state, health_result_t *out) {
    statecheck_card_t seen;
    memset(&seen, 0, sizeof(seen));
    sdcard_health_t sd;
    sdcard_health_state(&sd);
    seen.yield = card_yield(&sd);
    seen.mounted = sd.mounted;
    seen.locked = sd.locked;
    seen.fs = sd.fs;
    seen.encrypted = sd.encrypted;
    if (seen.yield || !seen.mounted) {
        statecheck_judge_card(&seen, out);
        return HEALTH_DONE;
    }
    seen.var_log = path_kind(LOGFILE_DIR);
    seen.certs = path_kind("/certs");
    seen.approved = approved();
    if (!seen.approved || seen.var_log != STATECHECK_PATH_DIR ||
        seen.certs == STATECHECK_PATH_FILE || seen.certs == STATECHECK_PATH_ERROR) {
        statecheck_judge_card(&seen, out);
        return HEALTH_DONE;
    }
    card_state_t *c = heap_zalloc(HEAP_FILES, sizeof(*c));
    if (!c) {
        out->status = HEALTH_SKIP;
        snprintf(out->text, sizeof(out->text), "no memory to read roots.pem; next run");
        return HEALTH_DONE;
    }
    c->seen = seen;
    c->generation = sd.generation;
    FRESULT fr = f_open(&c->file, CERTSTORE_FILE, FA_READ);
    if (fr != FR_OK || f_size(&c->file) > CERTSTORE_FILE_MAX) {
        if (fr == FR_OK)
            f_close(&c->file);
        c->seen.roots = fr == FR_NO_FILE || fr == FR_NO_PATH ? STATECHECK_ROOTS_MISSING
                      : fr == FR_OK ? STATECHECK_ROOTS_TOO_BIG : STATECHECK_ROOTS_UNREADABLE;
        statecheck_judge_card(&c->seen, out);
        heap_release(HEAP_FILES, c);
        return HEALTH_DONE;
    }
    files_hold();                       // a format or unmount waits for this, as for any file
    *state = c;
    if (wc_InitSha256(&c->sha) != 0) {
        c->seen.roots = STATECHECK_ROOTS_UNREADABLE;
        statecheck_judge_card(&c->seen, out);
        card_free(state, true);
        return HEALTH_DONE;
    }
    return HEALTH_MORE;
}

health_step_t statecheck_card_step(void **state, bool cancel, health_result_t *out) {
    if (cancel) {
        card_free(state, true);
        return HEALTH_DONE;
    }
    card_state_t *c = *state;
    if (!c)
        return card_start(state, out);
    // The card may have gone between steps (unmounted, remounted, taken by sd check).
    sdcard_health_t sd;
    sdcard_health_state(&sd);
    if (!sd.mounted || sd.busy || sd.generation != c->generation) {
        out->status = HEALTH_SKIP;
        snprintf(out->text, sizeof(out->text), "the card changed during the check; next run");
        card_free(state, false);        // that file is gone with its mount: nothing to close
        files_release();
        return HEALTH_DONE;
    }
    UINT got = 0;
    FRESULT fr = f_read(&c->file, c->buf, sizeof(c->buf), &got);
    if (fr != FR_OK || wc_Sha256Update(&c->sha, c->buf, got) != 0) {
        c->seen.roots = STATECHECK_ROOTS_UNREADABLE;
    } else if (got == sizeof(c->buf)) {
        return HEALTH_MORE;
    } else {
        uint8_t hash[32];
        c->seen.roots = wc_Sha256Final(&c->sha, hash) == 0 &&
                                !memcmp(hash, settings()->certs_hash, sizeof(hash))
                            ? STATECHECK_ROOTS_MATCH : STATECHECK_ROOTS_CHANGED;
    }
    statecheck_judge_card(&c->seen, out);
    card_free(state, true);
    return HEALTH_DONE;
}

// --- health state-dump -----------------------------------------------------------------------

#ifdef SOS_TEST_COMMANDS
#define DUMP_LINE 256               // the shell wraps it (statecheck_wrap())

__attribute__((format(printf, 3, 4)))
static void dump(health_print_fn print, void *arg, const char *fmt, ...) {
    char line[DUMP_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    print(arg, line);
}

static void dump_config(health_print_fn print, void *arg) {
    if (!settings_persistent()) {
        dump(print, arg, "config: flash not usable (the image overlaps the slots)");
        return;
    }
    config_slot_info_t info[2];
    for (unsigned i = 0; i < 2; i++) {
        uint64_t t0 = time_us_64();
        settings_config_slot(i, &info[i]);
        unsigned us = (unsigned)(time_us_64() - t0);
        dump(print, arg, "config slot %c: %-13s version %u length %u seq %lu  (%u us)", i ? 'B' : 'A',
             config_slot_kind_name(info[i].kind), info[i].version, info[i].length,
             (unsigned long)info[i].seq, us);
    }
    int use = config_in_use(info);
    dump(print, arg, "config in use: %s", use < 0 ? "none" : use ? "slot B" : "slot A");
}

static void dump_root(health_print_fn print, void *arg) {
    statecheck_root_t r;
    root_gather(&r);
    static const char *const src[] = {"OTP", "flash", "none"};
    static const char *const found[] = {"found", "none yet", "exhausted", "can't read",
                                        "bad marker"};
    dump(print, arg, "rootsecret: source %s; OTP: %s, page %u, secret ok %d, list locked %d "
         "(open page %u), soft lock %d; console-only %d",
         src[r.source], found[r.found], r.page, r.secret_ok, r.locks_ok, r.open_page,
         r.soft_locked, r.console_only);
    if (r.source == ROOTSRC_FLASH)
        dump(print, arg, "rootsecret: flash usable %d, state %d, degraded %d", r.flash_usable,
             r.flash_state, r.flash_degraded);
}

static void dump_otp(health_print_fn print, void *arg) {
    static statecheck_otp_t o;
    uint64_t t0 = time_us_64();
    otp_gather(&o);
    unsigned us = (unsigned)(time_us_64() - t0);
    dump(print, arg, "otp: latched CRITICAL 0x%08lx; read in %u us (rows 0x%03x-0x%03x, "
         "0x%03x-0x%03x, 0x%03x-0x%03x only)", (unsigned long)o.latched, us,
         STATECHECK_CRIT1_ROW, STATECHECK_CRIT1_ROW + STATECHECK_CRIT1_COPIES - 1,
         STATECHECK_FLAGS1_ROW, STATECHECK_FLAGS1_ROW + STATECHECK_FLAGS1_COPIES - 1,
         STATECHECK_BOOTKEY_ROW, STATECHECK_BOOTKEY_ROW + STATECHECK_BOOTKEY_ROWS - 1);
    char line[DUMP_LINE];
    int n = snprintf(line, sizeof(line), "otp: CRIT1");
    for (unsigned i = 0; i < STATECHECK_CRIT1_COPIES && n > 0 && (size_t)n < sizeof(line); i++)
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %06lx", (unsigned long)o.crit1[i]);
    print(arg, line);
    dump(print, arg, "otp: BOOT_FLAGS1 %06lx %06lx %06lx", (unsigned long)o.flags1[0],
         (unsigned long)o.flags1[1], (unsigned long)o.flags1[2]);
    uint32_t flags = bootkey_vote(o.flags1, STATECHECK_FLAGS1_COPIES, 2);
    for (unsigned s = 0; s < BOOTKEY_SLOTS; s++) {
        uint8_t hash[BOOTKEY_HASH_LEN];
        bootkey_slot_hash(o.key_ecc + s * BOOTKEY_SLOT_ROWS, hash);
        unsigned bad = 0, corrected = 0, unreadable = 0;
        for (unsigned k = 0; k < BOOTKEY_SLOT_ROWS; k++) {
            unsigned i = s * BOOTKEY_SLOT_ROWS + k;
            statecheck_row_t rr = statecheck_otp_row(o.key_raw[i], o.key_ecc[i]);
            bad += rr == STATECHECK_ROW_ECC_MISMATCH;
            corrected += rr == STATECHECK_ROW_CORRECTED;
            unreadable += rr == STATECHECK_ROW_UNREADABLE;
        }
        unsigned dev = statecheck_dev_key(hash);
        dump(print, arg, "otp: slot %u %02x%02x%02x%02x%02x%02x%02x%02x.. valid %u invalid %u "
             "%s; rows: %u ECC mismatch, %u corrected, %u unreadable", s, hash[0], hash[1],
             hash[2], hash[3], hash[4], hash[5], hash[6], hash[7], (unsigned)(flags >> s & 1u),
             (unsigned)(flags >> (STATECHECK_KEY_INVALID_LSB + s) & 1u),
             dev == 1 ? "dev key 1" : dev == 2 ? "dev key 2" : "-", bad, corrected, unreadable);
    }
    dump(print, arg, "otp: image signed %d, key %02x%02x%02x%02x%02x%02x%02x%02x..; build %s",
         o.image_signed, o.image_hash[0], o.image_hash[1], o.image_hash[2], o.image_hash[3],
         o.image_hash[4], o.image_hash[5], o.image_hash[6], o.image_hash[7],
         o.release ? "release (NDEBUG)" : "debug");
}

static void dump_failcount(health_print_fn print, void *arg) {
    statecheck_failcount_t f;
    failcount_gather(&f);
    static const char *const st[] = {"no sector", "erased", "marker", "other"};
    dump(print, arg, "failcount: usable %d, sector %s, attempts %u, cleared %u, stray %d, "
         "cleared over %d; count (RAM included) %u of capacity %u", f.usable, st[f.info.state],
         f.info.attempts, f.info.cleared, f.info.stray, f.info.cleared_over, f.count,
         (unsigned)FAILCOUNT_CAPACITY);
}

static void dump_hostkey(health_print_fn print, void *arg) {
    statecheck_hostkey_t h;
    hostkey_gather(&h);
    dump(print, arg, "hostkey: owner %d, key %d, gave up %d, unsealed %d, page as loaded %d, %s",
         h.have_owner, h.have_key, h.gave_up, h.unsealed, h.page_matches,
         h.fingerprint ? h.fingerprint : "(sshd not started)");
}

static void dump_card(health_print_fn print, void *arg) {
    sdcard_health_t sd;
    sdcard_health_state(&sd);
    dump(print, arg, "card: mounted %d, %s, encrypted %d, locked %d, busy %s, files open %d, "
         "roots approved %d (not read here)", sd.mounted, sd.fs ? sd.fs : "-", sd.encrypted,
         sd.locked, sd.busy ? sd.busy : "no", files_busy(), approved());
}

void statecheck_board_dump(health_print_fn print, void *arg) {
    dump_config(print, arg);
    dump_root(print, arg);
    dump_otp(print, arg);
    dump_failcount(print, arg);
    dump_hostkey(print, arg);
    dump_card(print, arg);
}
#endif
