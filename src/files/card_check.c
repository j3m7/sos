// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The `card` and `sd-bus` health checks' reads (docs/design/health.md §9.3, §9.9), moved from
// core/statecheck_board.c and core/countcheck_board.c (docs/design/build-features.md §4.3).
// - card: yields to whatever owns the card, never mounts one; hashes /certs/roots.pem 2 KB a
//   step, holding the files layer (files_hold) in between so a format or unmount waits.
//   core/statecheck.c judges.
// - sd-bus: the counters files/sdcard.c keeps in RAM, nothing sent to the card.
//   core/countcheck.c judges.

#include "files/card_check.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/sha256.h>

#include "core/countcheck.h"
#include "core/heap.h"
#include "core/heaptag.h"
#include "core/logfile.h"
#include "core/settings.h"
#include "core/statecheck.h"
#include "files/files.h"
#include "files/sdcard.h"
#include "tls/certstore.h"

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

health_step_t card_check_step(void **state, bool cancel, health_result_t *out) {
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

// --- sd-bus --------------------------------------------------------------------------------------

static void sd_gather(countcheck_sd_t *in, sdcard_counters_t *c) {
    sdcard_counters(c);
    const sdcard_ctr_t *i = &c->since_init;
    *in = (countcheck_sd_t){
        .ever_ok = c->ever_ok,
        .last_init_failed = c->last_init_failed,
        .khz = c->khz,
        .bytes = (i->sectors_read + i->sectors_written) * 512,
        .failed_reads = i->failed_reads,
        .failed_writes = i->failed_writes,
        .crc = i->crc,
        .cmd = i->cmd,
        .write = i->write,
        .timeouts = i->timeout,
    };
}

health_step_t sdbus_check_step(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (!cancel) {
        countcheck_sd_t in;
        sdcard_counters_t c;
        sd_gather(&in, &c);
        countcheck_judge_sd(&in, out);
    }
    return HEALTH_DONE;
}

#ifdef SOS_TEST_COMMANDS
// --- health state-dump's card and sd-bus lines ---------------------------------------------------

static uint32_t uptime_s(void) {
    return (uint32_t)(time_us_64() / 1000000);
}

#define DUMP_LINE 256

__attribute__((format(printf, 3, 4)))
static void dump(health_print_fn print, void *arg, const char *fmt, ...) {
    char line[DUMP_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    print(arg, line);
}

void card_check_dump(health_print_fn print, void *arg) {
    sdcard_health_t sd;
    sdcard_health_state(&sd);
    dump(print, arg, "card: mounted %d, %s, encrypted %d, locked %d, busy %s, files open %d, "
         "roots approved %d (not read here)", sd.mounted, sd.fs ? sd.fs : "-", sd.encrypted,
         sd.locked, sd.busy ? sd.busy : "no", files_busy(), approved());
}

static void dump_ctr(health_print_fn print, void *arg, const char *when, const sdcard_ctr_t *c) {
    dump(print, arg, "sd-bus %s: read %llu sectors, written %llu; failed reads %lu, writes %lu; "
         "retried crc %lu, cmd %lu, write %lu; timeouts %lu", when,
         (unsigned long long)c->sectors_read, (unsigned long long)c->sectors_written,
         (unsigned long)c->failed_reads, (unsigned long)c->failed_writes, (unsigned long)c->crc,
         (unsigned long)c->cmd, (unsigned long)c->write, (unsigned long)c->timeout);
}

void sdbus_check_dump(health_print_fn print, void *arg) {
    uint32_t now = uptime_s();

    countcheck_sd_t sd;
    sdcard_counters_t c;
    sd_gather(&sd, &c);
    dump(print, arg, "sd-bus: inits %lu (failed %lu), ever ok %d, last failed %d, %lu s ago; clock "
         "%lu kHz set, %lu in use; last error %d", (unsigned long)c.inits,
         (unsigned long)c.init_fails, c.ever_ok, c.last_init_failed,
         (unsigned long)(c.inits ? now - c.init_at_s : 0), (unsigned long)c.khz_set,
         (unsigned long)c.khz, c.last_error);
    dump_ctr(print, arg, "since init", &c.since_init);
    dump_ctr(print, arg, "since boot", &c.since_boot);
}
#endif
