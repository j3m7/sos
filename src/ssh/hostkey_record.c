// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The SSH host key's flash record (see hostkey_record.h). Pure C.

#include "ssh/hostkey_record.h"

#include <string.h>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/hmac.h>

#define MAGIC              0x4b534f53u  // "SOSK"
#define VERSION_PLAIN      1u
#define VERSION_SEALED_OLD 2u           // sealed under the base key alone
#define VERSION_SEALED     3u           // sealed under the base key and the owner secret

enum {
    OFF_MAGIC = 0, OFF_VERSION = 4, OFF_LEN = 8,
    HEADER_AAD = 12,                    // magic, version, length: authenticated by the seal
    OFF_SEALED = 12,
    OFF_PLAIN_CHECK = 12, OFF_PLAIN_KEY = 16,
    PLAIN_MAX_KEY = HOSTREC_PAGE - OFF_PLAIN_KEY,
};

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t fnv1a(const uint8_t *p, uint32_t n) {
    uint32_t h = 2166136261u;
    while (n--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

int hostrec_kind(const uint8_t page[HOSTREC_PAGE]) {
    if (get_le32(page + OFF_MAGIC) != MAGIC)
        return HOSTREC_NONE;
    uint32_t version = get_le32(page + OFF_VERSION), len = get_le32(page + OFF_LEN);
    if (version == VERSION_PLAIN && len > 0 && len <= PLAIN_MAX_KEY &&
        get_le32(page + OFF_PLAIN_CHECK) == fnv1a(page + OFF_PLAIN_KEY, len))
        return HOSTREC_PLAIN;
    if (version == VERSION_SEALED && len > 0 && len <= HOSTREC_MAX_KEY)
        return HOSTREC_SEALED;
    if (version == VERSION_SEALED_OLD && len > 0 && len <= HOSTREC_MAX_KEY)
        return HOSTREC_SEALED_OLD;
    return HOSTREC_NONE;
}

bool hostrec_open(const uint8_t page[HOSTREC_PAGE], const sealer_t *sealer, bool refuse_plain,
                  uint8_t *der, uint32_t *len) {
    uint32_t n = get_le32(page + OFF_LEN);
    memset(der, 0, HOSTREC_PAGE);
    *len = 0;
    switch (hostrec_kind(page)) {
    case HOSTREC_PLAIN:
        if (refuse_plain)
            return false;
        memcpy(der, page + OFF_PLAIN_KEY, n);
        break;
    case HOSTREC_SEALED:
    case HOSTREC_SEALED_OLD:
        if (!sealer || !sealer->open(sealer->ctx, page, HEADER_AAD, page + OFF_SEALED,
                                     n + SEALER_OVERHEAD, der)) {
            memset(der, 0, HOSTREC_PAGE);
            return false;
        }
        break;
    default:
        return false;
    }
    *len = n;
    return true;
}

bool hostrec_seal(uint8_t page[HOSTREC_PAGE], const sealer_t *sealer, const uint8_t *der,
                  uint32_t len) {
    memset(page, 0xff, HOSTREC_PAGE);
    if (sealer && len > 0 && len <= HOSTREC_MAX_KEY) {
        put_le32(page + OFF_MAGIC, MAGIC);
        put_le32(page + OFF_VERSION, VERSION_SEALED);
        put_le32(page + OFF_LEN, len);
        if (sealer->seal(sealer->ctx, page, HEADER_AAD, der, len, page + OFF_SEALED))
            return true;
    }
    memset(page, 0, HOSTREC_PAGE);
    return false;
}

bool hostrec_owner_key(uint8_t out[HOSTREC_KEY_LEN], const uint8_t base[HOSTREC_KEY_LEN],
                       const uint8_t owner[32]) {
    Hmac hmac;
    bool init = wc_HmacInit(&hmac, NULL, INVALID_DEVID) == 0;
    bool ok = init && wc_HmacSetKey(&hmac, WC_SHA256, base, HOSTREC_KEY_LEN) == 0 &&
              wc_HmacUpdate(&hmac, owner, 32) == 0 && wc_HmacFinal(&hmac, out) == 0;
    if (init)
        wc_HmacFree(&hmac);
    volatile uint8_t *v = (volatile uint8_t *)&hmac;
    for (size_t i = 0; i < sizeof(hmac); i++)
        v[i] = 0;
    if (!ok)
        memset(out, 0, HOSTREC_KEY_LEN);
    return ok;
}

hostkey_action_t hostrec_decide(int kind, bool have_base, bool have_owner, bool console_only) {
    if (!have_base)
        // No root secret: nothing sealed opens and nothing new could be sealed. An open
        // board with a plain key from old firmware goes on using it.
        return kind == HOSTREC_PLAIN && !console_only ? HOSTKEY_USE_PLAIN_UNSEALED
                                                     : HOSTKEY_NONE_NO_KEYS;
    if (!have_owner)
        // Not set up yet. A key made now couldn't be tied to an owner; one is made when the
        // board is set up. What is stored stays as it is until then.
        return HOSTKEY_NONE_NO_OWNER;
    switch (kind) {
    case HOSTREC_SEALED:
        return HOSTKEY_OPEN;
    case HOSTREC_SEALED_OLD:
        // Sealed under the board's key alone, so it would have survived a wipe: a console-only
        // board doesn't carry it over to what may be a new owner.
        return console_only ? HOSTKEY_MAKE_NEW : HOSTKEY_TAKE_OVER_OLD;
    case HOSTREC_PLAIN:
        return console_only ? HOSTKEY_MAKE_NEW : HOSTKEY_TAKE_OVER_PLAIN;
    default:
        return HOSTKEY_MAKE_NEW;
    }
}
