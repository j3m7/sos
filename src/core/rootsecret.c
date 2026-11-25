// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The root secret's flash record (see rootsecret.h). Pure C.

#include "core/rootsecret.h"

#include <string.h>

enum { OFF_MAGIC = 0, OFF_VERSION = 4, OFF_SECRET = 8, OFF_CHECK = 40, RECORD_END = 44 };
_Static_assert(OFF_CHECK == OFF_SECRET + ROOTSECRET_LEN, "layout");

#define MAGIC   0x52534f53u     // "SOSR"
#define VERSION 1u

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

static int page_state(const uint8_t *page) {
    bool blank = true;
    for (size_t i = 0; i < ROOTSECRET_PAGE && blank; i++)
        blank = page[i] == 0xff;
    if (blank)
        return ROOTSECRET_NONE;
    if (get_le32(page + OFF_MAGIC) == MAGIC && get_le32(page + OFF_VERSION) == VERSION &&
        get_le32(page + OFF_CHECK) == fnv1a(page + OFF_SECRET, ROOTSECRET_LEN))
        return ROOTSECRET_OK;
    return ROOTSECRET_INVALID;
}

int rootsecret_state(const rootsecret_store_t *store) {
    return page_state(store->read(store->ctx));
}

int rootsecret_ensure(const rootsecret_store_t *store, bool *created) {
    if (created)
        *created = false;
    int state = rootsecret_state(store);
    if (state != ROOTSECRET_NONE)
        return state;

    uint8_t page[ROOTSECRET_PAGE];
    memset(page, 0xff, sizeof(page));
    if (!store->random(store->ctx, page + OFF_SECRET, ROOTSECRET_LEN)) {
        wipe(page, sizeof(page));
        return ROOTSECRET_NONE;
    }
    put_le32(page + OFF_MAGIC, MAGIC);
    put_le32(page + OFF_VERSION, VERSION);
    put_le32(page + OFF_CHECK, fnv1a(page + OFF_SECRET, ROOTSECRET_LEN));
    // What counts is what reads back: stored if it's exactly what was made. Anything else (a
    // failed write that left the page blank, or one that left part of a record) is reported as
    // the state it left.
    store->write(store->ctx, page);
    bool ok = !memcmp(store->read(store->ctx), page, sizeof(page));
    wipe(page, sizeof(page));
    if (!ok)
        return rootsecret_state(store);
    if (created)
        *created = true;
    return ROOTSECRET_OK;
}

bool rootsecret_read(const rootsecret_store_t *store, uint8_t out[ROOTSECRET_LEN]) {
    const uint8_t *page = store->read(store->ctx);
    if (page_state(page) != ROOTSECRET_OK) {
        memset(out, 0, ROOTSECRET_LEN);
        return false;
    }
    memcpy(out, page + OFF_SECRET, ROOTSECRET_LEN);
    return true;
}

bool rootsecret_forget(const rootsecret_store_t *store) {
    return store->write(store->ctx, NULL) && rootsecret_state(store) == ROOTSECRET_NONE;
}
