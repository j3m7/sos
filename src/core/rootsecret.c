// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The root secret's flash record (see rootsecret.h). Pure C.

#include "core/rootsecret.h"
#include "core/rootsecret_priv.h"

#include <string.h>

enum { OFF_MAGIC = 0, OFF_VERSION = 4, OFF_SECRET = 8, OFF_CHECK = 40, RECORD_END = 44 };
_Static_assert(OFF_CHECK == OFF_SECRET + ROOTSECRET_LEN, "layout");

#define MAGIC   0x52534f53u     // "SOSR"
#define VERSION 1u

enum { PAGE_A = 0, PAGE_B = 1, COPIES = 2 };    // A is written last: see rootsecret.h

enum { P_BLANK, P_VALID, P_BAD };

static void wipe(void *p, size_t len) {
    volatile uint8_t *v = p;
    while (len--)
        *v++ = 0;
}

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t fnv1a(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    while (n--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

static bool blank(const uint8_t *p, size_t len) {
    for (size_t i = 0; i < len; i++)
        if (p[i] != 0xff)
            return false;
    return true;
}

static int page_kind(const uint8_t *page) {
    if (blank(page, ROOTSECRET_PAGE))
        return P_BLANK;
    if (get_le32(page + OFF_MAGIC) == MAGIC && get_le32(page + OFF_VERSION) == VERSION &&
        get_le32(page + OFF_CHECK) == fnv1a(page + OFF_SECRET, ROOTSECRET_LEN) &&
        blank(page + RECORD_END, ROOTSECRET_PAGE - RECORD_END))
        return P_VALID;
    return P_BAD;
}

// What the sector holds. `use` (if not NULL) is the page to read the secret from when OK.
enum {
    S_NONE,             // both pages blank
    S_UNFINISHED_B,     // A blank, B a valid copy: the first write stopped before A
    S_TORN_B,           // A blank, B part of a record and nothing else in the sector: start over
    S_OK,
    S_INVALID,
};

static int classify(const uint8_t *sector, const uint8_t **use, bool *degraded) {
    const uint8_t *a = sector + PAGE_A * ROOTSECRET_PAGE, *b = sector + PAGE_B * ROOTSECRET_PAGE;
    int ka = page_kind(a), kb = page_kind(b);
    if (use)
        *use = NULL;
    if (degraded)
        *degraded = false;
    if (ka == P_BLANK) {
        // A is written last and never erased alone, so no secret from this sector was ever used.
        if (kb == P_BLANK)
            return S_NONE;
        if (kb == P_VALID)
            return S_UNFINISHED_B;
        // Part of a record: only what a cut-off write of page B leaves (nothing past the record,
        // nothing in the rest of the sector) is started over. Anything else isn't ours to erase.
        if (blank(b + RECORD_END, ROOTSECRET_PAGE - RECORD_END) &&
            blank(sector + COPIES * ROOTSECRET_PAGE, ROOTSECRET_SECTOR - COPIES * ROOTSECRET_PAGE))
            return S_TORN_B;
        return S_INVALID;
    }
    if (ka == P_VALID && kb == P_VALID &&
        memcmp(a + OFF_SECRET, b + OFF_SECRET, ROOTSECRET_LEN) != 0)
        return S_INVALID;               // two different secrets: no telling which was used
    if (ka != P_VALID && kb != P_VALID)
        return S_INVALID;
    if (use)
        *use = ka == P_VALID ? a : b;
    if (degraded)
        *degraded = ka != P_VALID || kb != P_VALID;
    return S_OK;
}

static int public_state(int s) {
    return s == S_OK ? ROOTSECRET_OK : s == S_INVALID ? ROOTSECRET_INVALID : ROOTSECRET_NONE;
}

int rootsecret_state(const rootsecret_store_t *store) {
    return public_state(classify(store->read(store->ctx), NULL, NULL));
}

bool rootsecret_degraded(const rootsecret_store_t *store) {
    bool degraded;
    return classify(store->read(store->ctx), NULL, &degraded) == S_OK && degraded;
}

// Program `data` into a page that reads blank; true if it then reads back exactly.
static bool program_page(const rootsecret_store_t *store, unsigned page, const uint8_t *data) {
    const uint8_t *at = store->read(store->ctx) + page * ROOTSECRET_PAGE;
    if (!blank(at, ROOTSECRET_PAGE))
        return false;
    store->program(store->ctx, page, data);     // what counts is what reads back
    return !memcmp(store->read(store->ctx) + page * ROOTSECRET_PAGE, data, ROOTSECRET_PAGE);
}

int rootsecret_ensure(const rootsecret_store_t *store, bool *created) {
    uint8_t page[ROOTSECRET_PAGE];
    const uint8_t *use;
    bool made = false;
    if (created)
        *created = false;

    int s = classify(store->read(store->ctx), NULL, NULL);
    if (s == S_TORN_B) {
        if (!store->erase(store->ctx))
            return ROOTSECRET_NONE;
        s = classify(store->read(store->ctx), NULL, NULL);
        if (s != S_NONE)
            return public_state(s);
    }
    if (s == S_NONE) {
        memset(page, 0xff, sizeof(page));
        bool ok = store->random(store->ctx, page + OFF_SECRET, ROOTSECRET_LEN);
        if (ok) {
            put_le32(page + OFF_MAGIC, MAGIC);
            put_le32(page + OFF_VERSION, VERSION);
            put_le32(page + OFF_CHECK, fnv1a(page + OFF_SECRET, ROOTSECRET_LEN));
            ok = program_page(store, PAGE_B, page);
        }
        wipe(page, sizeof(page));
        if (!ok)
            return public_state(classify(store->read(store->ctx), NULL, NULL));
        made = true;
        s = S_UNFINISHED_B;
    }
    if (s == S_UNFINISHED_B) {
        // Copy B to A. What counts is what the sector then holds: A still blank leaves "none"
        // (the secret isn't used, the next boot tries again); A written, even only in part,
        // makes B's secret the one in use.
        memcpy(page, store->read(store->ctx) + PAGE_B * ROOTSECRET_PAGE, sizeof(page));
        program_page(store, PAGE_A, page);
        wipe(page, sizeof(page));
        s = classify(store->read(store->ctx), NULL, NULL);
        if (s == S_OK && created)
            *created = made;
        return public_state(s);
    }
    if (s != S_OK)
        return ROOTSECRET_INVALID;

    // One valid copy and the other page blank (a record made before there were two copies):
    // fill it in. A failure only leaves the board with one copy, as before.
    classify(store->read(store->ctx), &use, NULL);
    unsigned other = use == store->read(store->ctx) ? PAGE_B : PAGE_A;
    if (blank(store->read(store->ctx) + other * ROOTSECRET_PAGE, ROOTSECRET_PAGE)) {
        memcpy(page, use, sizeof(page));
        program_page(store, other, page);
        wipe(page, sizeof(page));
    }
    return public_state(classify(store->read(store->ctx), NULL, NULL));
}

bool rootsecret_read(const rootsecret_store_t *store, uint8_t out[ROOTSECRET_LEN]) {
    const uint8_t *use;
    if (classify(store->read(store->ctx), &use, NULL) != S_OK) {
        memset(out, 0, ROOTSECRET_LEN);
        return false;
    }
    memcpy(out, use + OFF_SECRET, ROOTSECRET_LEN);
    return true;
}

bool rootsecret_forget(const rootsecret_store_t *store) {
    return store->erase(store->ctx) && rootsecret_state(store) == ROOTSECRET_NONE;
}
