// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "core/settings.h"

#include <string.h>

#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/stdlib.h"

#include "core/keyprov.h"
#include "core/log.h"
#include "core/pwverify.h"
#include "core/random.h"
#include "core/seal.h"

// Slot A and B: the two sectors below the host key (the last sector, see hostkey.c).
#define SLOT_OFFSET(slot) (PICO_FLASH_SIZE_BYTES - (3 - (slot)) * FLASH_SECTOR_SIZE)

_Static_assert(CONFIG_SLOT_SIZE == FLASH_SECTOR_SIZE, "a config slot is one flash sector");

static sos_config_t live;
static bool persistent;

// --- flash backend for config_store_t ------------------------------------------------------------

static const uint8_t *flash_slot_read(void *ctx, unsigned slot) {
    (void)ctx;
    return (const uint8_t *)(XIP_BASE + SLOT_OFFSET(slot));
}

typedef struct {
    unsigned slot;
    const uint8_t *data;
    size_t len;                 // multiple of FLASH_PAGE_SIZE
} write_job_t;

static void do_write(void *param) {
    const write_job_t *job = param;
    flash_range_erase(SLOT_OFFSET(job->slot), FLASH_SECTOR_SIZE);
    if (job->len)
        flash_range_program(SLOT_OFFSET(job->slot), job->data, job->len);
}

static bool flash_slot_write(void *ctx, unsigned slot, const void *data, size_t len) {
    (void)ctx;
    static uint8_t page_buf[CONFIG_SLOT_SIZE];      // programming works in whole pages
    if (!persistent || len > sizeof(page_buf))
        return false;
    size_t padded = (len + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE;
    memset(page_buf, 0xff, padded);
    memcpy(page_buf, data, len);
    // Interrupts (Wi-Fi, USB) pause while the sector is erased and programmed: tens of ms.
    write_job_t job = { slot, page_buf, padded };
    return flash_safe_execute(do_write, &job, 1000) == PICO_OK;
}

// The key the configuration is sealed under (KEYPROV_CONFIG_SEAL), derived once in
// settings_init(), before the key provider locks. It stays here, in RAM, for as long as the
// board runs: every save needs it. Nothing copies it elsewhere.
static seal_ctx_t seal = { .random = random_bytes };
static const sealer_t sealer = { seal_seal, seal_open, &seal };

// An open board: plain records (from older firmware, or a provisioning image) are loaded, and
// sealed at once. A secured board will refuse them (refuse_plain; issue #64 step 4).
static const config_store_t store = { flash_slot_read, flash_slot_write, NULL, &sealer, false };

// --- console password --------------------------------------------------------------------------------

// PBKDF2 iterations for a newly set password. The target is about 300 ms per check on the
// RP2350; this count is an estimate that has NOT been measured on the board yet (issue #64).
// It's stored with each verifier, so changing it doesn't affect a password already set.
#ifndef SOS_CONSOLE_PW_ITERATIONS
#define SOS_CONSOLE_PW_ITERATIONS 4000
#endif
// Iterations per main-loop pass while a password is checked.
#define CHECK_SLICE 100

_Static_assert(PWV_SALT_LEN == CONFIG_CONSOLE_SALT_LEN && PWV_LEN == CONFIG_CONSOLE_VERIFIER_LEN &&
               PWV_ITERATIONS_MIN == CONFIG_CONSOLE_ITERATIONS_MIN &&
               PWV_ITERATIONS_MAX == CONFIG_CONSOLE_ITERATIONS_MAX, "config.h mirrors pwverify.h");

// The key the verifier is keyed with: in RAM while the board runs (a password can be checked
// or set at any time). Nothing copies it elsewhere.
static uint8_t pw_key[PWV_LEN];
static bool have_pw_key;
static pwv_job_t pw_job;
static bool pw_checking;

bool settings_console_has_password(void) {
    return live.console_iterations != 0;
}

const char *settings_console_set_password(const char *password) {
    const char *err = config_check_console_password(password);
    if (err)
        return err;
    if (!have_pw_key)
        return "this board has no key to protect a password with (see `rootsecret`)";
    uint8_t salt[PWV_SALT_LEN], verifier[PWV_LEN];
    uint64_t t0 = time_us_64();
    if (!random_bytes(NULL, salt, sizeof(salt)) ||
        !pwv_compute(password, strlen(password), salt, SOS_CONSOLE_PW_ITERATIONS, pw_key, verifier))
        return "making the password's verifier failed";
    // How long one check of this password costs (the same derivation), for tuning the count.
    log_msg(SOS_LOG_INFO, "config: password verifier made in %lu ms (%u iterations)",
            (unsigned long)((time_us_64() - t0) / 1000), (unsigned)SOS_CONSOLE_PW_ITERATIONS);
    live.console_iterations = SOS_CONSOLE_PW_ITERATIONS;
    memcpy(live.console_salt, salt, sizeof(salt));
    memcpy(live.console_verifier, verifier, sizeof(verifier));
    memset(verifier, 0, sizeof(verifier));
    return NULL;
}

bool settings_console_check_begin(const char *password) {
    settings_console_check_abort();
    pw_checking = have_pw_key && settings_console_has_password() &&
                  pwv_start(&pw_job, password, strlen(password), live.console_salt,
                            live.console_iterations);
    return pw_checking;
}

int settings_console_check_step(void) {
    if (!pw_checking)
        return -1;
    if (!pwv_step(&pw_job, CHECK_SLICE))
        return 0;
    uint8_t verifier[PWV_LEN];
    pw_checking = false;
    bool right = pwv_finish(&pw_job, pw_key, verifier) &&
                 pwv_equal(verifier, live.console_verifier);
    memset(verifier, 0, sizeof(verifier));
    return right ? 1 : -1;
}

void settings_console_check_abort(void) {
    if (pw_checking)
        pwv_abort(&pw_job);
    pw_checking = false;
}

// A plain record (provisioning) may bring the console password to set: turn it into the
// verifier and forget it, before the record is sealed.
static void take_provisioned_password(void) {
    if (!live.console_password[0])
        return;
    const char *err = settings_console_set_password(live.console_password);
    memset(live.console_password, 0, sizeof(live.console_password));
    if (err)
        log_msg(SOS_LOG_ERR, "config: the provisioned console password wasn't set: %s", err);
    else
        log_msg(SOS_LOG_NOTICE, "config: console password set from the provisioned "
                                "configuration");
}

// --- owner secret ------------------------------------------------------------------------------------

bool settings_has_owner_secret(void) {
    for (unsigned i = 0; i < sizeof(live.owner_secret); i++)
        if (live.owner_secret[i])
            return true;
    return false;
}

// Before a save: a configuration without an owner secret gets a new one. Only when it can be
// stored sealed; never logged.
static void ensure_owner_secret(void) {
    if (settings_has_owner_secret() || !seal.have_key)
        return;
    if (random_bytes(NULL, live.owner_secret, sizeof(live.owner_secret)))
        log_msg(SOS_LOG_NOTICE, "config: this configuration is a new owner for encrypted SD "
                                "cards (cards encrypted under an earlier one stay locked)");
    else
        log_msg(SOS_LOG_ERR, "config: making the owner secret failed (no random numbers)");
}

// --- public API ----------------------------------------------------------------------------------

void settings_init(void) {
    extern char __flash_binary_end;
    persistent = (uintptr_t)&__flash_binary_end <= XIP_BASE + SLOT_OFFSET(0);
    if (!persistent) {
        log_msg(SOS_LOG_ERR, "config: firmware overlaps the config area; settings won't be saved");
        config_defaults(&live);
        return;
    }

    seal.have_key = keyprov_derive(KEYPROV_CONFIG_SEAL, seal.key);
    have_pw_key = keyprov_derive(KEYPROV_CONSOLE_PASSWORD, pw_key);
    if (!seal.have_key)
        log_msg(SOS_LOG_ERR, "config: no key to seal the configuration with (see `rootsecret`): "
                             "a sealed configuration can't be read, and nothing can be saved");

    uint32_t seq;
    if (config_load(&store, &live, &seq)) {
        log_msg(SOS_LOG_INFO, "config: loaded (revision %lu)", (unsigned long)seq);
        if (config_stored(&store) == CONFIG_STORED_PLAIN) {
            // From older firmware or a provisioning image: store it sealed, and erase the
            // plain copy (config_save does both). A plain record has no owner secret, so this
            // configuration gets a new one.
            ensure_owner_secret();
            take_provisioned_password();
            if (config_save(&store, &live))
                log_msg(SOS_LOG_NOTICE, "config: the saved configuration was unsealed (older "
                                        "firmware, or provisioning); it's now stored sealed");
            else
                log_msg(SOS_LOG_ERR, "config: sealing the saved configuration failed; it stays "
                                     "unsealed in flash");
        }
        if (!config_scrub_plain(&store))
            log_msg(SOS_LOG_ERR, "config: an unsealed configuration record is still in flash "
                                 "(erasing it failed)");
        // A sealed configuration from before the owner secret: give it one now, so "no owner
        // secret" only ever means that nothing is saved.
        if (!settings_has_owner_secret() && seal.have_key &&
            config_stored(&store) == CONFIG_STORED_SEALED)
            settings_save();
        return;
    }
    if (config_sealed_unopened(&store))
        log_msg(SOS_LOG_ERR, "config: a sealed configuration is present and doesn't open "
                             "(sealed under another root secret, or changed); it's left as it "
                             "is and the board starts unconfigured");
    // Nothing saved (a new board, after `config reset`, or records this firmware can't read):
    // no network and no trusted keys until someone sets it up. The firmware carries no
    // configuration of its own.
    config_defaults(&live);
    log_msg(SOS_LOG_NOTICE, "config: none saved; run 'setup' on the console (or provision the board)");
}

sos_config_t *settings(void) {
    return &live;
}

bool settings_save(void) {
    ensure_owner_secret();
    if (!config_save(&store, &live)) {
        log_msg(SOS_LOG_ERR, "config: saving to flash failed");
        return false;
    }
    log_msg(SOS_LOG_INFO, "config: saved");
    return true;
}

bool settings_reset(void) {
    bool ok = persistent && config_erase(&store);
    config_defaults(&live);
    log_msg(SOS_LOG_NOTICE, "config: reset%s", ok ? "" : " (erasing the saved one failed)");
    return ok;
}

bool settings_persistent(void) {
    return persistent;
}
