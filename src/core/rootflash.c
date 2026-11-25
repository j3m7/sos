// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "core/rootflash.h"

#include <string.h>

#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/stdlib.h"

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/random.h>

#include "core/keyprov.h"
#include "core/log.h"
#include "core/rootsecret.h"
#include "core/rootsecret_priv.h"     // the one place that reads the secret out

// The fourth sector from the end: below the two configuration slots (core/settings.c) and the
// key sector (core/keysector.c).
#define ROOT_OFFSET (PICO_FLASH_SIZE_BYTES - 4 * FLASH_SECTOR_SIZE)

_Static_assert(ROOTSECRET_PAGE == FLASH_PAGE_SIZE, "the record is one flash page");
_Static_assert(ROOTSECRET_SECTOR == FLASH_SECTOR_SIZE, "the store is one flash sector");

static bool usable;

// --- flash backend for rootsecret_store_t ---------------------------------------------------------

static const uint8_t *store_read(void *ctx) {
    (void)ctx;
    return (const uint8_t *)(XIP_BASE + ROOT_OFFSET);
}

typedef struct {
    unsigned page;
    const uint8_t *data;        // NULL: erase the sector instead
} flash_job_t;

static void do_flash(void *param) {
    const flash_job_t *job = param;
    if (job->data)              // a blank page: programmed without erasing anything
        flash_range_program(ROOT_OFFSET + job->page * FLASH_PAGE_SIZE, job->data, FLASH_PAGE_SIZE);
    else
        flash_range_erase(ROOT_OFFSET, FLASH_SECTOR_SIZE);
}

static bool store_program(void *ctx, unsigned page, const uint8_t *data) {
    (void)ctx;
    flash_job_t job = { page, data };
    return usable && page < FLASH_SECTOR_SIZE / FLASH_PAGE_SIZE &&
           flash_safe_execute(do_flash, &job, 1000) == PICO_OK;
}

static bool store_erase(void *ctx) {
    (void)ctx;
    flash_job_t job = { 0, NULL };
    return usable && flash_safe_execute(do_flash, &job, 1000) == PICO_OK;
}

static bool store_random(void *ctx, uint8_t *out, size_t len) {
    (void)ctx;
    WC_RNG rng;
    if (wc_InitRng(&rng) != 0)
        return false;
    int rc = wc_RNG_GenerateBlock(&rng, out, (word32)len);
    wc_FreeRng(&rng);
    return rc == 0;
}

static const rootsecret_store_t store = {
    store_read, store_program, store_erase, store_random, NULL,
};

// --- key provider backend ---------------------------------------------------------------------------

static bool backend_root(void *ctx, uint8_t out[KEYPROV_ROOT_LEN]) {
    (void)ctx;
    return usable && rootsecret_read(&store, out);
}

// Nothing to lock on an open board: the flash stays readable. The provider's own flag is the
// lock. (A secured board locks its OTP page here.)
static const keyprov_backend_t backend = { backend_root, NULL, NULL };

_Static_assert(KEYPROV_ROOT_LEN == ROOTSECRET_LEN, "the provider's root is the stored secret");

// --- public API -------------------------------------------------------------------------------------

int rootflash_init(void) {
    extern char __flash_binary_end;
    usable = (uintptr_t)&__flash_binary_end <= XIP_BASE + ROOT_OFFSET;
    keyprov_init(&backend);
    if (!usable) {
        log_msg(SOS_LOG_ERR, "rootsecret: the firmware image overlaps the root secret's sector; "
                             "there are no keys");
        return ROOTSECRET_INVALID;
    }
    bool created;
    int state = rootsecret_ensure(&store, &created);
    if (created)
        log_msg(SOS_LOG_NOTICE, "rootsecret: made this board's root secret (kept in flash: the "
                                "board is open)");
    else if (state == ROOTSECRET_NONE)
        log_msg(SOS_LOG_ERR, "rootsecret: making the root secret failed; there are no keys");
    else if (state == ROOTSECRET_INVALID)
        log_msg(SOS_LOG_ERR, "rootsecret: the root secret's record can't be used (damaged, or from "
                             "newer firmware); it's kept as it is, and there are no keys: "
                             "`rootsecret` explains");
    else if (rootsecret_degraded(&store))
        log_msg(SOS_LOG_WARNING, "rootsecret: one of the root secret's two copies is damaged; "
                                 "the other is in use");
    return state;
}

int rootflash_state(void) {
    return usable ? rootsecret_state(&store) : ROOTSECRET_INVALID;
}

bool rootflash_degraded(void) {
    return usable && rootsecret_degraded(&store);
}

bool rootflash_usable(void) {
    return usable;
}

bool rootflash_forget(void) {
    bool ok = usable && rootsecret_forget(&store);
    log_msg(ok ? SOS_LOG_NOTICE : SOS_LOG_ERR, "rootsecret: %s",
            ok ? "the root secret was cleared" : "clearing the root secret failed");
    return ok;
}
