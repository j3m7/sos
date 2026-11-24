// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "core/settings.h"

#include <string.h>

#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/stdlib.h"

#include "core/log.h"
#include "sos_defaults.h"

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

// --- build-time defaults -------------------------------------------------------------------------

static void seed_defaults(sos_config_t *c) {
    config_defaults(c);
#ifdef SOS_WIFI_SSID
    if (!config_check_ssid(SOS_WIFI_SSID))
        strcpy(c->wifi_ssid, SOS_WIFI_SSID);
#endif
#ifdef SOS_WIFI_PASSWORD
    if (!config_check_password(SOS_WIFI_PASSWORD))
        strcpy(c->wifi_password, SOS_WIFI_PASSWORD);
#endif
#ifdef SOS_HOSTNAME
    if (!config_check_hostname(SOS_HOSTNAME))
        strcpy(c->hostname, SOS_HOSTNAME);
#endif
#ifdef SOS_SSH_USER
    if (!config_check_user(SOS_SSH_USER))
        strcpy(c->ssh_user, SOS_SSH_USER);
#endif
#ifdef SOS_SSH_AUTHORIZED_KEY
    config_key_t key;
    const char *err = config_key_parse(SOS_SSH_AUTHORIZED_KEY, &key);
    if (err)
        log_msg(SOS_LOG_WARNING, "config: build-time SOS_SSH_AUTHORIZED_KEY ignored: %s", err);
    else
        config_key_add(c, &key);
#endif
}

// --- public API ----------------------------------------------------------------------------------

void settings_init(void) {
    extern char __flash_binary_end;
    persistent = (uintptr_t)&__flash_binary_end <= XIP_BASE + SLOT_OFFSET(0);
    if (!persistent) {
        log_msg(SOS_LOG_ERR, "config: firmware overlaps the config area; settings won't be saved");
        seed_defaults(&live);
        return;
    }

    uint32_t seq;
    if (config_load(&store, &live, &seq)) {
        log_msg(SOS_LOG_INFO, "config: loaded (revision %lu)", (unsigned long)seq);
        return;
    }
    seed_defaults(&live);
    log_msg(SOS_LOG_NOTICE, "config: none saved yet; using %s",
            live.wifi_ssid[0] || live.key_count ? "build-time defaults" : "empty defaults");
    if (live.wifi_ssid[0] || live.key_count)
        settings_save();
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
    seed_defaults(&live);
    if (ok && (live.wifi_ssid[0] || live.key_count))
        ok = settings_save();
    log_msg(SOS_LOG_NOTICE, "config: reset to %s", ok ? "defaults" : "defaults (not saved)");
    return ok;
}

bool settings_persistent(void) {
    return persistent;
}
