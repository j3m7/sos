// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "core/keysector.h"
#include "core/log.h"
#include "ssh/hostkey.h"

#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "pico/stdlib.h"

#include <wolfssh/ssh.h>
#include <wolfssh/keygen.h>

// The host key is the first page of the key sector (core/keysector.h), which survives
// reflashing.
#define HOSTKEY_MAGIC   0x4b534f53u   // "SOSK"
#define HOSTKEY_VERSION 1u
#define HOSTKEY_MAX_DER (FLASH_PAGE_SIZE - 16)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t len;
    uint32_t check;                 // FNV-1a over der[0..len)
    uint8_t der[HOSTKEY_MAX_DER];
} hostkey_record_t;

_Static_assert(sizeof(hostkey_record_t) == FLASH_PAGE_SIZE, "record must be one flash page");

static hostkey_record_t ram_copy;

static uint32_t fnv1a(const uint8_t *p, uint32_t n) {
    uint32_t h = 2166136261u;
    while (n--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

static const hostkey_record_t *flash_record(void) {
    return (const hostkey_record_t *)keysector_page(KEYSECTOR_HOSTKEY);
}

static bool record_valid(const hostkey_record_t *r) {
    return r->magic == HOSTKEY_MAGIC && r->version == HOSTKEY_VERSION &&
           r->len > 0 && r->len <= HOSTKEY_MAX_DER && r->check == fnv1a(r->der, r->len);
}

static bool flash_write(const hostkey_record_t *r) {
    return keysector_write(KEYSECTOR_HOSTKEY, (const uint8_t *)r);
}

bool hostkey_get(const uint8_t **der, uint32_t *len) {
    const hostkey_record_t *fr = flash_record();

    if (!record_valid(fr)) {
        log_msg(SOS_LOG_NOTICE, "hostkey: no valid host key in flash, generating Ed25519 key...");
        memset(&ram_copy, 0xff, sizeof(ram_copy));
        int n = wolfSSH_MakeEd25519Key(ram_copy.der, sizeof(ram_copy.der), WOLFSSH_ED25519KEY);
        if (n <= 0) {
            log_msg(SOS_LOG_ERR, "hostkey: key generation failed (%d)", n);
            return false;
        }
        ram_copy.magic = HOSTKEY_MAGIC;
        ram_copy.version = HOSTKEY_VERSION;
        ram_copy.len = (uint32_t)n;
        ram_copy.check = fnv1a(ram_copy.der, ram_copy.len);

        if (!flash_write(&ram_copy) || !record_valid(fr)) {
            log_msg(SOS_LOG_ERR, "hostkey: failed to store host key in flash");
            return false;
        }
        log_msg(SOS_LOG_NOTICE, "hostkey: stored new host key (%lu bytes)", (unsigned long)ram_copy.len);
        memset(&ram_copy, 0, sizeof(ram_copy));
    }

    *der = fr->der;
    *len = fr->len;
    return true;
}

bool hostkey_erase(void) {
    return keysector_write(KEYSECTOR_HOSTKEY, NULL);      // keeps the device key
}
