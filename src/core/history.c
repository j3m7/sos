// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See history.h.

#include "core/history.h"

#include <string.h>

#include "core/resetwhy.h"

static const uint8_t magic[4] = {'S', 'O', 'S', 'H'};
#define HDR_VERSION 4                   // the header's version byte
#define CONFIRM_BYTE 13

static uint16_t crc16(const uint8_t *p, unsigned n) {
    uint16_t crc = 0xffff;              // CRC-16/CCITT-FALSE
    while (n--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++)
            crc = (uint16_t)(crc & 0x8000 ? crc << 1 ^ 0x1021 : crc << 1);
    }
    return crc;
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static bool all_ff(const uint8_t *p, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        if (p[i] != 0xff)
            return false;
    return true;
}

static const uint8_t *slot_at(const uint8_t *sector, unsigned slot) {
    return sector + HISTORY_PAGE + slot * HISTORY_RECORD;
}

static bool known_kind(uint8_t k) {
    return k == HISTORY_KIND_BOOT || k == HISTORY_KIND_RTC || k == HISTORY_KIND_TEST;
}

void history_encode(const history_rec_t *rec, uint8_t out[HISTORY_RECORD]) {
    memset(out, 0, HISTORY_RECORD);
    out[0] = rec->kind;
    out[1] = rec->kind == HISTORY_KIND_RTC ? (uint8_t)rec->cal_steps : rec->reason;
    out[2] = (uint8_t)rec->boot_no;
    out[3] = (uint8_t)(rec->boot_no >> 8);
    put32(out + 4, rec->uptime_s);
    put32(out + 8, rec->wall);
    out[12] = rec->extra;
    out[CONFIRM_BYTE] = rec->confirmed ? 0x00 : 0xff;
    uint16_t crc = crc16(out, 13);
    out[14] = (uint8_t)crc;
    out[15] = (uint8_t)(crc >> 8);
}

static bool decode(const uint8_t *in, history_rec_t *out) {
    if (!known_kind(in[0]))
        return false;
    uint16_t crc = (uint16_t)(in[14] | in[15] << 8);
    if (crc != crc16(in, 13))
        return false;
    out->kind = in[0];
    out->reason = in[1];
    out->cal_steps = (int8_t)in[1];
    out->boot_no = (uint16_t)(in[2] | in[3] << 8);
    out->uptime_s = get32(in + 4);
    out->wall = get32(in + 8);
    out->extra = in[12];
    out->confirmed = in[CONFIRM_BYTE] == 0x00;
    return true;
}

void history_scan(const uint8_t *sector, history_scan_t *out) {
    memset(out, 0, sizeof(*out));
    if (!sector) {
        out->state = HISTORY_ABSENT;
        return;
    }
    if (all_ff(sector, HISTORY_SECTOR)) {
        out->state = HISTORY_ERASED;
        return;
    }
    if (memcmp(sector, magic, sizeof(magic)) != 0) {
        out->state = HISTORY_FOREIGN;
        return;
    }
    if (sector[HDR_VERSION] > HISTORY_VERSION) {
        out->state = HISTORY_NEWER;
        return;
    }
    if (sector[HDR_VERSION] != HISTORY_VERSION) {
        out->state = HISTORY_FOREIGN;
        return;
    }
    out->state = HISTORY_OURS;
    out->next = HISTORY_SLOTS;
    for (unsigned i = 0; i < HISTORY_SLOTS; i++) {
        const uint8_t *p = slot_at(sector, i);
        if (out->next == HISTORY_SLOTS) {
            history_rec_t r;
            if (all_ff(p, HISTORY_RECORD)) {
                out->next = i;
            } else if (decode(p, &r)) {
                out->valid++;
                out->boots += r.kind == HISTORY_KIND_BOOT;
            } else {
                out->damaged++;
            }
        } else if (!all_ff(p, HISTORY_RECORD)) {
            out->stray++;
        }
    }
}

bool history_get(const uint8_t *sector, unsigned slot, history_rec_t *out) {
    history_scan_t s;
    history_scan(sector, &s);
    if (s.state != HISTORY_OURS || slot >= s.next)
        return false;
    return decode(slot_at(sector, slot), out);
}

bool history_slot(const uint8_t *sector, unsigned slot, history_rec_t *out) {
    return sector && slot < HISTORY_SLOTS && decode(slot_at(sector, slot), out);
}

unsigned history_list(const uint8_t *sector, uint8_t kind, history_rec_t *out, unsigned max) {
    history_scan_t s;
    history_scan(sector, &s);
    if (s.state != HISTORY_OURS)
        return 0;
    unsigned n = 0;
    for (unsigned i = s.next; i-- > 0 && n < max;) {
        history_rec_t r;
        if (decode(slot_at(sector, i), &r) && (!kind || r.kind == kind))
            out[n++] = r;
    }
    return n;
}

uint16_t history_next_boot_no(const uint8_t *sector) {
    history_rec_t r;
    return history_list(sector, HISTORY_KIND_BOOT, &r, 1) ? (uint16_t)(r.boot_no + 1) : 0;
}

// Program `len` bytes at `offset` (within one page), the rest of the page left as it is, and read
// them back.
static bool write_at(const history_store_t *store, unsigned offset, const uint8_t *data,
                     unsigned len) {
    uint8_t page[HISTORY_PAGE];
    memset(page, 0xff, sizeof(page));
    memcpy(page + offset % HISTORY_PAGE, data, len);
    if (!store->program(store->ctx, offset / HISTORY_PAGE, page))
        return false;
    const uint8_t *now = store->read(store->ctx);
    return now && memcmp(now + offset, data, len) == 0;
}

static bool write_header(const history_store_t *store) {
    uint8_t h[HISTORY_RECORD];
    memset(h, 0xff, sizeof(h));
    memcpy(h, magic, sizeof(magic));
    h[HDR_VERSION] = HISTORY_VERSION;
    return write_at(store, 0, h, sizeof(h));
}

static bool write_slot(const history_store_t *store, unsigned slot, const uint8_t rec[16]) {
    return write_at(store, HISTORY_PAGE + slot * HISTORY_RECORD, rec, HISTORY_RECORD);
}

// Erase, header, and the newest boot records and the newest confirmed RTC record back, in their
// order. Returns the next free slot, or -1.
static int start_again(const history_store_t *store, const uint8_t *sector) {
    uint8_t keep[HISTORY_CARRY_BOOTS + 1][HISTORY_RECORD];
    unsigned n = 0, boots = 0;
    bool rtc = false;
    if (sector) {
        history_scan_t s;
        history_scan(sector, &s);
        for (unsigned i = s.state == HISTORY_OURS ? s.next : 0; i-- > 0;) {
            history_rec_t r;
            if (!decode(slot_at(sector, i), &r))
                continue;
            bool take = (r.kind == HISTORY_KIND_BOOT && boots < HISTORY_CARRY_BOOTS) ||
                        (r.kind == HISTORY_KIND_RTC && !rtc && r.confirmed);
            if (!take)
                continue;
            boots += r.kind == HISTORY_KIND_BOOT;
            rtc = rtc || r.kind == HISTORY_KIND_RTC;
            memcpy(keep[n++], slot_at(sector, i), HISTORY_RECORD);
        }
    }
    if (!store->erase(store->ctx) || !write_header(store))
        return -1;
    unsigned at = 0;
    for (unsigned k = n; k-- > 0;) {        // oldest first, as they were
        if (!write_slot(store, at++, keep[k]))
            return -1;
    }
    return (int)at;
}

int history_append(const history_store_t *store, const history_rec_t *rec) {
    const uint8_t *sector = store->read(store->ctx);
    if (!sector)
        return -1;
    history_scan_t s;
    history_scan(sector, &s);
    int slot;
    switch (s.state) {
    case HISTORY_NEWER:
    case HISTORY_ABSENT:
        return -1;
    case HISTORY_ERASED:
        if (!write_header(store))
            return -1;
        slot = 0;
        break;
    case HISTORY_FOREIGN:
        slot = start_again(store, NULL);
        break;
    default:
        // Full, or something written past the end of the list (a stray record, which would
        // become part of the list once records reached it): start again.
        slot = s.next < HISTORY_SLOTS && !s.stray ? (int)s.next : start_again(store, sector);
        break;
    }
    if (slot < 0 || slot >= (int)HISTORY_SLOTS)
        return -1;
    uint8_t bytes[HISTORY_RECORD];
    history_encode(rec, bytes);
    return write_slot(store, (unsigned)slot, bytes) ? slot : -1;
}

bool history_confirm(const history_store_t *store, unsigned slot) {
    if (slot >= HISTORY_SLOTS)
        return false;
    uint8_t zero = 0x00;
    return write_at(store, HISTORY_PAGE + slot * HISTORY_RECORD + CONFIRM_BYTE, &zero, 1);
}

static bool short_run(const history_rec_t *boots, unsigned n, unsigned i, uint32_t stable_s) {
    if (boots[i].uptime_s != HISTORY_NONE)
        return boots[i].uptime_s < stable_s;
    // The run that ended started at the previous boot (boots[i + 1]) and ended before this one.
    if (i + 1 < n && boots[i].wall != HISTORY_NONE && boots[i + 1].wall != HISTORY_NONE &&
        boots[i].wall >= boots[i + 1].wall)
        return boots[i].wall - boots[i + 1].wall < stable_s;
    return false;
}

unsigned history_short_runs(const history_rec_t *boots, unsigned n, uint32_t stable_s) {
    unsigned k = 0;
    while (k < n && boots[k].reason != RESETWHY_REBOOT && short_run(boots, n, k, stable_s))
        k++;
    return k;
}
