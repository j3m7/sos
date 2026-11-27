// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "core/keysector.h"

#include <string.h>

#include "hardware/flash.h"
#include "pico/stdlib.h"

#include "core/flashop.h"
#include "core/log.h"

#define KEYSECTOR_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

_Static_assert(KEYSECTOR_PAGES * FLASH_PAGE_SIZE <= FLASH_SECTOR_SIZE, "pages fit the sector");

static uint8_t pages[KEYSECTOR_PAGES * FLASH_PAGE_SIZE];

const uint8_t *keysector_page(unsigned page) {
    return (const uint8_t *)(XIP_BASE + KEYSECTOR_OFFSET + page * FLASH_PAGE_SIZE);
}

static void do_write(void *param) {
    (void)param;
    flash_range_erase(KEYSECTOR_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(KEYSECTOR_OFFSET, pages, sizeof(pages));
}

bool keysector_write(unsigned page, const uint8_t *data) {
    extern char __flash_binary_end;
    if (page >= KEYSECTOR_PAGES)
        return false;
    if ((uintptr_t)&__flash_binary_end > XIP_BASE + KEYSECTOR_OFFSET) {
        log_msg(SOS_LOG_ERR, "keys: firmware image overlaps the key sector");
        return false;
    }
    memcpy(pages, keysector_page(0), sizeof(pages));
    if (data)
        memcpy(pages + page * FLASH_PAGE_SIZE, data, FLASH_PAGE_SIZE);
    else
        memset(pages + page * FLASH_PAGE_SIZE, 0xff, FLASH_PAGE_SIZE);
    bool ok = flashop_run(do_write, NULL) &&
              !memcmp(keysector_page(0), pages, sizeof(pages));
    memset(pages, 0, sizeof(pages));        // keys don't stay in RAM
    return ok;
}
