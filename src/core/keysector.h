// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_KEYSECTOR_H
#define SOS_KEYSECTOR_H

#include <stdbool.h>
#include <stdint.h>

// The last 4 KB flash sector holds the device's keys, one flash page (256 bytes) each, well
// clear of the firmware image and of the configuration sectors below it. `picotool load`,
// `config reset` and provisioning don't touch it, so the keys survive reflashing.
enum {
    KEYSECTOR_HOSTKEY,      // the SSH host key (ssh/hostkey.c)
    KEYSECTOR_DEVKEY,       // the device key that unlocks encrypted SD cards (core/devkey.c)
    KEYSECTOR_PAGES
};

// The page as stored (read through the XIP window).
const uint8_t *keysector_page(unsigned page);

// Replace one page (FLASH_PAGE_SIZE bytes; NULL erases it), keeping the others. The sector is
// erased and rewritten, so a power cut during the write can lose every key in it: called only
// when a key is created or erased.
bool keysector_write(unsigned page, const uint8_t *data);

#endif
