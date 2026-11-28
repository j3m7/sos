// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See history.h: the flash backend of the history sector, and the boot record.

#include "core/history.h"

#include <time.h>

#include "hardware/flash.h"
#include "pico/stdlib.h"

#include "core/flashlayout.h"
#include "core/flashop.h"
#include "core/glitchdet.h"
#include "core/log.h"
#include "core/resetwhy.h"
#include "time/timesync.h"

#define HISTORY_OFFSET FLASHLAYOUT_HISTORY_OFFSET

_Static_assert(HISTORY_SECTOR == FLASH_SECTOR_SIZE, "the store is one flash sector");
_Static_assert(HISTORY_PAGE == FLASH_PAGE_SIZE, "it is programmed a flash page at a time");

static bool usable, usable_known, write_failed_logged;

static bool check_usable(void) {
    if (!usable_known) {
        extern char __flash_binary_end;
        usable = (uintptr_t)&__flash_binary_end <= XIP_BASE + HISTORY_OFFSET;
        usable_known = true;
    }
    return usable;
}

const uint8_t *history_sector(void) {
    return check_usable() ? (const uint8_t *)(XIP_BASE + HISTORY_OFFSET) : NULL;
}

static const uint8_t *store_read(void *ctx) {
    (void)ctx;
    return history_sector();
}

typedef struct {
    unsigned page;
    const uint8_t *data;                // NULL: erase the sector instead
} flash_job_t;

static void do_flash(void *param) {
    const flash_job_t *job = param;
    if (job->data)
        flash_range_program(HISTORY_OFFSET + job->page * FLASH_PAGE_SIZE, job->data,
                            FLASH_PAGE_SIZE);
    else
        flash_range_erase(HISTORY_OFFSET, FLASH_SECTOR_SIZE);
}

static bool store_program(void *ctx, unsigned page, const uint8_t *data) {
    (void)ctx;
    flash_job_t job = {page, data};
    return check_usable() && page < FLASH_SECTOR_SIZE / FLASH_PAGE_SIZE && flashop_run(do_flash, &job);
}

static bool store_erase(void *ctx) {
    (void)ctx;
    flash_job_t job = {0, NULL};
    return check_usable() && flashop_run(do_flash, &job);
}

static const history_store_t store = {NULL, store_read, store_program, store_erase};

static void write_failed(const char *what) {
    if (write_failed_logged)
        return;
    write_failed_logged = true;
    log_msg(SOS_LOG_KERN | SOS_LOG_WARNING, "history: writing %s failed; not recorded", what);
}

void history_boot(void) {
    const uint8_t *sector = history_sector();
    if (!sector)
        return;                         // `resets` says so
    history_scan_t s;
    history_scan(sector, &s);
    if (s.state == HISTORY_NEWER)
        return;
    resetwhy_t why = resetwhy();
    history_rec_t rec = {
        .kind = HISTORY_KIND_BOOT,
        .reason = (uint8_t)why,
        .boot_no = history_next_boot_no(sector),
        .uptime_s = glitchdet_prev_uptime_s(),
        .wall = timesync_clock_set() ? (uint32_t)time(NULL) : HISTORY_NONE,
        .extra = why == RESETWHY_GLITCH ? (uint8_t)glitchdet_last_triggers() : 0,
    };
    if (history_append(&store, &rec) < 0)
        write_failed("the boot record");
}

int history_rtc_begin(uint32_t t, int cal_steps, uint8_t load) {
    if (!history_sector())
        return -1;
    history_rec_t rec = {
        .kind = HISTORY_KIND_RTC,
        .cal_steps = (int8_t)cal_steps,
        .rtc_load = load,
        .uptime_s = 0,
        .wall = t,
    };
    int slot = history_append(&store, &rec);
    if (slot < 0)
        write_failed("the RTC record");
    return slot;
}

void history_rtc_done(int slot) {
    if (slot >= 0 && !history_confirm(&store, (unsigned)slot))
        write_failed("the RTC record's confirmation");
}

#ifdef SOS_TEST_COMMANDS
unsigned history_fill(void) {
    unsigned n = 0;
    for (;;) {
        history_scan_t s;
        history_scan(history_sector(), &s);
        if (s.state != HISTORY_OURS || s.next >= HISTORY_SLOTS)
            return n;
        history_rec_t rec = {.kind = HISTORY_KIND_TEST, .uptime_s = HISTORY_NONE,
                             .wall = HISTORY_NONE};
        if (history_append(&store, &rec) < 0)
            return n;
        n++;
    }
}
#endif
