// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See failflash.h.

#include "core/failflash.h"

#include "hardware/flash.h"
#include "pico/stdlib.h"

#include "core/failcount.h"
#include "core/flashop.h"
#include "core/log.h"

// The fifth sector from the end: below the root secret's (core/rootflash.c).
#define FAIL_OFFSET (PICO_FLASH_SIZE_BYTES - 5 * FLASH_SECTOR_SIZE)

_Static_assert(FAILCOUNT_SECTOR == FLASH_SECTOR_SIZE, "the store is one flash sector");
_Static_assert(FAILCOUNT_PAGE == FLASH_PAGE_SIZE, "it is programmed a flash page at a time");

static bool usable, write_failed_logged;

static const uint8_t *store_read(void *ctx) {
    (void)ctx;
    if (!usable) {                      // never read the firmware image as a count
        static const uint8_t blank[FAILCOUNT_SECTOR];   // zeros: no marker, so a count of zero
        return blank;
    }
    return (const uint8_t *)(XIP_BASE + FAIL_OFFSET);
}

typedef struct {
    unsigned page;
    const uint8_t *data;                // NULL: erase the sector instead
} flash_job_t;

static void do_flash(void *param) {
    const flash_job_t *job = param;
    if (job->data)
        flash_range_program(FAIL_OFFSET + job->page * FLASH_PAGE_SIZE, job->data, FLASH_PAGE_SIZE);
    else
        flash_range_erase(FAIL_OFFSET, FLASH_SECTOR_SIZE);
}

static bool store_program(void *ctx, unsigned page, const uint8_t *data) {
    (void)ctx;
    flash_job_t job = { page, data };
    return usable && page < FLASH_SECTOR_SIZE / FLASH_PAGE_SIZE &&
           flashop_run(do_flash, &job);
}

static bool store_erase(void *ctx) {
    (void)ctx;
    flash_job_t job = { 0, NULL };
    return usable && flashop_run(do_flash, &job);
}

static const failcount_store_t store = { NULL, store_read, store_program, store_erase };

unsigned failflash_init(void) {
    extern char __flash_binary_end;
    usable = (uintptr_t)&__flash_binary_end <= XIP_BASE + FAIL_OFFSET;
    failcount_init(&store);
    if (!usable)
        log_msg(SOS_LOG_AUTH | SOS_LOG_ERR,
                "console: the firmware image overlaps the wrong-password count's "
                "sector; the count is kept only until the next reboot");
    return failcount_get();
}

bool failflash_usable(void) { return usable; }

unsigned failflash_count(void) { return failcount_get(); }

// Said once per boot: every further answer would say the same.
static void write_failed(const char *what) {
    if (write_failed_logged || !usable)
        return;
    write_failed_logged = true;
    log_msg(SOS_LOG_AUTH | SOS_LOG_ERR,
            "console: %s the wrong-password count in flash failed; it is counted in "
            "memory until the next reboot", what);
}

void failflash_attempt(void) {
    if (!failcount_attempt())
        write_failed("writing");
}

void failflash_clear(void) {
    if (!failcount_clear())
        write_failed("clearing");
}
