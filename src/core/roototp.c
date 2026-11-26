// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See roototp.h.

#include "core/roototp.h"

#include <string.h>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/hmac.h>

#define UNREADABLE 0xffffffffu

static void wipe(void *p, size_t len) {
    volatile uint8_t *v = p;
    while (len--)
        *v++ = 0;
}

roototp_page_t roototp_page(const roototp_reader_t *r, unsigned index) {
    bool blank = true;
    for (unsigned row = 0; row < ROOTOTP_PAGE_ROWS; row++) {
        uint32_t raw = r->raw(r->ctx, ROOTOTP_ROW(index, row));
        if (raw == UNREADABLE)          // a raw row has 24 bits
            return ROOTOTP_UNREADABLE;
        if (raw & 0xffffffu)
            blank = false;
    }
    if (blank)
        return ROOTOTP_BLANK;
    return r->ecc(r->ctx, ROOTOTP_ROW(index, ROOTOTP_MARKER_ROW)) == ROOTOTP_MARKER
               ? ROOTOTP_COMPLETE
               : ROOTOTP_SPENT;
}

roototp_find_t roototp_find(const roototp_reader_t *r, unsigned *index) {
    for (unsigned i = 0; i < ROOTOTP_PAGES; i++) {
        switch (roototp_page(r, i)) {
        case ROOTOTP_COMPLETE:
            *index = i;
            return ROOTOTP_FOUND;
        case ROOTOTP_BLANK:
            *index = i;
            return ROOTOTP_NONE_YET;
        case ROOTOTP_UNREADABLE:
            *index = i;
            return ROOTOTP_CANT_READ;
        case ROOTOTP_SPENT:
            break;                      // skipped for good; the next page decides
        }
    }
    *index = ROOTOTP_PAGES;
    return ROOTOTP_EXHAUSTED;
}

bool roototp_check(const uint8_t secret[ROOTOTP_SECRET_LEN], uint8_t check[ROOTOTP_CHECK_LEN]) {
    static const char label[] = "SOS root secret check v1";
    uint8_t mac[WC_SHA256_DIGEST_SIZE];
    Hmac hmac;
    bool ok = wc_HmacInit(&hmac, NULL, INVALID_DEVID) == 0;
    if (ok) {
        ok = wc_HmacSetKey(&hmac, WC_SHA256, secret, ROOTOTP_SECRET_LEN) == 0 &&
             wc_HmacUpdate(&hmac, (const byte *)label, sizeof(label) - 1) == 0 &&
             wc_HmacFinal(&hmac, mac) == 0;
        wc_HmacFree(&hmac);
    }
    if (ok)
        memcpy(check, mac, ROOTOTP_CHECK_LEN);
    else
        memset(check, 0, ROOTOTP_CHECK_LEN);
    wipe(mac, sizeof(mac));
    return ok;
}

bool roototp_read(const roototp_reader_t *r, unsigned index, uint8_t out[ROOTOTP_SECRET_LEN]) {
    uint16_t rows[ROOTOTP_SECRET_ROWS + ROOTOTP_CHECK_ROWS];
    uint8_t stored[ROOTOTP_CHECK_LEN], want[ROOTOTP_CHECK_LEN];
    bool ok = index < ROOTOTP_PAGES &&
              r->careful(r->ctx, ROOTOTP_ROW(index, 0), ROOTOTP_SECRET_ROWS + ROOTOTP_CHECK_ROWS,
                         rows);
    if (ok) {
        for (unsigned k = 0; k < ROOTOTP_SECRET_ROWS; k++) {
            out[2 * k] = (uint8_t)rows[k];
            out[2 * k + 1] = (uint8_t)(rows[k] >> 8);
        }
        for (unsigned k = 0; k < ROOTOTP_CHECK_ROWS; k++) {
            stored[2 * k] = (uint8_t)rows[ROOTOTP_CHECK_ROW + k];
            stored[2 * k + 1] = (uint8_t)(rows[ROOTOTP_CHECK_ROW + k] >> 8);
        }
        ok = roototp_check(out, want);
        // Compare without stopping at the first difference.
        uint8_t diff = 0;
        for (unsigned i = 0; i < ROOTOTP_CHECK_LEN; i++)
            diff |= (uint8_t)(stored[i] ^ want[i]);
        ok = ok && diff == 0;
    }
    wipe(rows, sizeof(rows));
    wipe(want, sizeof(want));
    if (!ok)
        wipe(out, ROOTOTP_SECRET_LEN);
    return ok;
}

rootsrc_t roototp_choose(roototp_find_t found, bool otp_secret_ok, bool sealed_only) {
    if (found == ROOTOTP_FOUND)
        // A secret was written to OTP. If it can't be used, nothing else stands in for it:
        // not on a sealed-only board, and not on an open one either.
        return otp_secret_ok ? ROOTSRC_OTP : ROOTSRC_NONE;
    if (sealed_only)
        return ROOTSRC_NONE;            // OTP or not at all
    if (found == ROOTOTP_CANT_READ)
        return ROOTSRC_NONE;
    return ROOTSRC_FLASH;               // an open board with no secret in OTP
}
