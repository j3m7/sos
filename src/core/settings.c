// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "core/settings.h"

#include <string.h>

#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/stdlib.h"

#include "core/log.h"

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

static const config_store_t store = { flash_slot_read, flash_slot_write, NULL };

// --- public API ----------------------------------------------------------------------------------

void settings_init(void) {
    extern char __flash_binary_end;
    persistent = (uintptr_t)&__flash_binary_end <= XIP_BASE + SLOT_OFFSET(0);
    if (!persistent) {
        log_msg(SOS_LOG_ERR, "config: firmware overlaps the config area; settings won't be saved");
        config_defaults(&live);
        return;
    }

    uint32_t seq;
    if (config_load(&store, &live, &seq)) {
        log_msg(SOS_LOG_INFO, "config: loaded (revision %lu)", (unsigned long)seq);
        return;
    }
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
