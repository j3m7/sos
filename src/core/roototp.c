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
        // Never written. If the firmware could never write it either, it is no place for a
        // secret: passed over like a spent page.
        return roototp_secure_can_write(r->lock1(r->ctx, ROOTOTP_FIRST_PAGE + index))
                   ? ROOTOTP_BLANK
                   : ROOTOTP_SPENT;
    // The marker row: blank means the write stopped before it; the marker means complete;
    // anything else is neither, and must not be taken for "spent" and passed over.
    unsigned marker_row = ROOTOTP_ROW(index, ROOTOTP_MARKER_ROW);
    if ((r->raw(r->ctx, marker_row) & 0xffffffu) == 0)
        return ROOTOTP_SPENT;
    return r->ecc(r->ctx, marker_row) == ROOTOTP_MARKER ? ROOTOTP_COMPLETE : ROOTOTP_DAMAGED;
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
        case ROOTOTP_DAMAGED:
            *index = i;
            return ROOTOTP_BAD_MARKER;
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

// The lock byte is stored three times in the row; each bit by two of three.
static uint32_t lock_byte(uint32_t lock1_raw) {
    uint32_t a = lock1_raw & 0xff, b = lock1_raw >> 8 & 0xff, c = lock1_raw >> 16 & 0xff;
    return (a & b) | (a & c) | (b & c);
}

bool roototp_bootloader_can_write(uint32_t lock1_raw) {
    if (lock1_raw == UNREADABLE)
        return true;                    // can't tell: treated as not locked
    return (lock_byte(lock1_raw) >> 4 & 3u) == 0;   // LOCK_BL: 0 = reads and writes permitted
}

bool roototp_secure_can_write(uint32_t lock1_raw) {
    if (lock1_raw == UNREADABLE)
        return false;                   // can't tell: don't count on writing it
    return (lock_byte(lock1_raw) & 3u) == 0;        // LOCK_S: 0 = read-write
}

bool roototp_locks_ok(const roototp_reader_t *r, unsigned index, unsigned *open_page) {
    for (unsigned i = 0; i <= index && i < ROOTOTP_PAGES; i++) {
        unsigned page = ROOTOTP_FIRST_PAGE + i;
        if (roototp_bootloader_can_write(r->lock1(r->ctx, page))) {
            if (open_page)
                *open_page = page;
            return false;
        }
    }
    return true;
}

rootsrc_t roototp_choose(roototp_find_t found, bool otp_secret_ok, bool locks_ok,
                         bool sealed_only) {
    if (found == ROOTOTP_FOUND) {
        // A secret was written to OTP. If it can't be used, nothing else stands in for it:
        // not on a sealed-only board, and not on an open one either.
        if (!otp_secret_ok)
            return ROOTSRC_NONE;
        // With a page of the list still writable from the bootloader, the secret found may be
        // one somebody completed from a spent page. A sealed-only board doesn't use it.
        return sealed_only && !locks_ok ? ROOTSRC_NONE : ROOTSRC_OTP;
    }
    if (sealed_only)
        return ROOTSRC_NONE;            // OTP or not at all
    if (found == ROOTOTP_CANT_READ || found == ROOTOTP_BAD_MARKER)
        return ROOTSRC_NONE;
    return ROOTSRC_FLASH;               // an open board with no secret in OTP
}
