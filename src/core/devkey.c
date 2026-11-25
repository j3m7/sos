// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "core/devkey.h"

#include <string.h>

#include "hardware/flash.h"

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/random.h>

#include "core/keysector.h"
#include "core/log.h"

#define DEVKEY_MAGIC   0x44534f53u      // "SOSD"
#define DEVKEY_VERSION 1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t key[DEVKEY_LEN];
    uint32_t check;                     // FNV-1a over key
    uint8_t pad[FLASH_PAGE_SIZE - 44];
} devkey_record_t;

_Static_assert(sizeof(devkey_record_t) == FLASH_PAGE_SIZE, "record must be one flash page");

static bool ready;

static uint32_t fnv1a(const uint8_t *p, uint32_t n) {
    uint32_t h = 2166136261u;
    while (n--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

static const devkey_record_t *record(void) {
    return (const devkey_record_t *)keysector_page(KEYSECTOR_DEVKEY);
}

static bool valid(const devkey_record_t *r) {
    return r->magic == DEVKEY_MAGIC && r->version == DEVKEY_VERSION &&
           r->check == fnv1a(r->key, DEVKEY_LEN);
}

bool devkey_init(void) {
    const devkey_record_t *r = record();
    if (valid(r))
        return ready = true;
    if (r->magic == DEVKEY_MAGIC)
        log_msg(SOS_LOG_ERR, "devkey: the device key is damaged; encrypted SD cards made with it "
                             "can only be read with their saved key (sd key)");

    static devkey_record_t fresh;
    WC_RNG rng;
    memset(&fresh, 0xff, sizeof(fresh));
    if (wc_InitRng(&rng) != 0) {
        log_msg(SOS_LOG_ERR, "devkey: no random number generator");
        return false;
    }
    int rc = wc_RNG_GenerateBlock(&rng, fresh.key, DEVKEY_LEN);
    wc_FreeRng(&rng);
    if (rc != 0) {
        log_msg(SOS_LOG_ERR, "devkey: generating the key failed (%d)", rc);
        return false;
    }
    fresh.magic = DEVKEY_MAGIC;
    fresh.version = DEVKEY_VERSION;
    fresh.check = fnv1a(fresh.key, DEVKEY_LEN);
    bool ok = keysector_write(KEYSECTOR_DEVKEY, (const uint8_t *)&fresh) && valid(r);
    memset(&fresh, 0, sizeof(fresh));
    if (!ok) {
        log_msg(SOS_LOG_ERR, "devkey: storing the device key failed");
        return false;
    }
    log_msg(SOS_LOG_NOTICE, "devkey: made a new device key (for encrypted SD cards)");
    return ready = true;
}

const uint8_t *devkey(void) {
    return ready ? record()->key : NULL;
}
