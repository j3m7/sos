// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The SSH host key's flash record (see hostkey_record.h). Pure C.

#include "ssh/hostkey_record.h"

#include <string.h>

#define MAGIC          0x4b534f53u      // "SOSK"
#define VERSION_PLAIN  1u
#define VERSION_SEALED 2u

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
    return HOSTREC_NONE;
}

bool hostrec_open(const uint8_t page[HOSTREC_PAGE], const sealer_t *sealer, uint8_t *der,
                  uint32_t *len) {
    uint32_t n = get_le32(page + OFF_LEN);
    memset(der, 0, HOSTREC_PAGE);
    *len = 0;
    switch (hostrec_kind(page)) {
    case HOSTREC_PLAIN:
        memcpy(der, page + OFF_PLAIN_KEY, n);
        break;
    case HOSTREC_SEALED:
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
