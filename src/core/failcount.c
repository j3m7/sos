// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See failcount.h.

#include "core/failcount.h"

#include <string.h>

#define ATTEMPTS_OFFSET (1 * FAILCOUNT_PAGE)
#define CLEARED_OFFSET  (ATTEMPTS_OFFSET + FAILCOUNT_TRACK_BYTES)
_Static_assert(CLEARED_OFFSET + FAILCOUNT_TRACK_BYTES <= FAILCOUNT_SECTOR, "two tracks fit");

static const uint8_t marker[8] = { 'S', 'O', 'S', 'F', 'A', 'I', 'L', '1' };

static const failcount_store_t *st;
static unsigned attempts, cleared;      // bits cleared in each track, as last read or written
static unsigned extra;                  // counted in RAM only: flash couldn't take them
static bool have_marker;

// A track's count: its leading cleared bits, lowest bit of the first byte first. Bits are
// cleared in that order, so anything after the first set bit isn't part of the count.
static unsigned track_count(const uint8_t *track) {
    unsigned n = 0;
    for (size_t i = 0; i < FAILCOUNT_TRACK_BYTES; i++) {
        uint8_t b = track[i];
        if (b == 0) {
            n += 8;
            continue;
        }
        while (!(b & 1u)) {
            n++;
            b >>= 1;
        }
        break;
    }
    return n;
}

static void load(void) {
    const uint8_t *s = st->read(st->ctx);
    have_marker = memcmp(s, marker, sizeof(marker)) == 0;
    attempts = cleared = 0;
    if (have_marker) {
        attempts = track_count(s + ATTEMPTS_OFFSET);
        cleared = track_count(s + CLEARED_OFFSET);
        if (cleared > attempts)         // can't happen by our writes: don't make it a negative
            cleared = attempts;
    }
}

// Clear bits [from, to) of the track at `offset`, a page at a time. A page is programmed with
// all ones except the bits to clear, which leaves every other bit as it is.
static bool clear_bits(size_t offset, unsigned from, unsigned to) {
    while (from < to) {
        size_t byte = offset + from / 8, page = byte / FAILCOUNT_PAGE;
        uint8_t buf[FAILCOUNT_PAGE];
        memset(buf, 0xff, sizeof(buf));
        unsigned page_end_bit = (unsigned)(((page + 1) * FAILCOUNT_PAGE - offset) * 8);
        unsigned stop = to < page_end_bit ? to : page_end_bit;
        for (unsigned bit = from; bit < stop; bit++)
            buf[offset + bit / 8 - page * FAILCOUNT_PAGE] &= (uint8_t)~(1u << (bit % 8));
        if (!st->program(st->ctx, (unsigned)page, buf))
            return false;
        // The flash call reports nothing, so a write that didn't take would go unnoticed:
        // read the bits back. Every bit that was to be cleared must now read 0.
        const uint8_t *now = st->read(st->ctx) + page * FAILCOUNT_PAGE;
        for (unsigned i = 0; i < FAILCOUNT_PAGE; i++)
            if (now[i] & (uint8_t)~buf[i])
                return false;
        from = stop;
    }
    return true;
}

// Erase and start the sector again holding `carry` outstanding attempts. If power is lost
// between the erase and the writes the count is lost with it; that window opens once in
// FAILCOUNT_CAPACITY checks, or when the sector held something else.
static bool restart(unsigned carry) {
    uint8_t buf[FAILCOUNT_PAGE];
    if (!st->erase(st->ctx))
        return false;
    have_marker = false;
    attempts = cleared = 0;
    const uint8_t *s = st->read(st->ctx);
    for (size_t i = 0; i < FAILCOUNT_SECTOR; i++)
        if (s[i] != 0xff)               // an erase that didn't take: the tracks can't be trusted
            return false;
    memset(buf, 0xff, sizeof(buf));
    memcpy(buf, marker, sizeof(marker));
    if (!st->program(st->ctx, 0, buf) || memcmp(st->read(st->ctx), marker, sizeof(marker)))
        return false;
    have_marker = true;
    if (!clear_bits(ATTEMPTS_OFFSET, 0, carry))
        return false;
    attempts = carry;
    return true;
}

void failcount_init(const failcount_store_t *store) {
    st = store;
    extra = 0;
    load();
}

unsigned failcount_get(void) {
    return attempts - cleared + extra;
}

bool failcount_attempt(void) {
    unsigned want = failcount_get() + 1;
    bool ok = true, restarted = false;
    if (!have_marker) {
        ok = restart(0);
    } else if (attempts >= FAILCOUNT_CAPACITY) {
        unsigned carry = attempts - cleared;
        ok = restart(carry > FAILCOUNT_CARRY_MAX ? FAILCOUNT_CARRY_MAX : carry);
        restarted = true;
    }
    if (ok && clear_bits(ATTEMPTS_OFFSET, attempts, attempts + 1)) {
        attempts++;
        if (restarted)
            want = failcount_get();     // the cap applied: that is the count now
    } else {
        ok = false;
        load();                         // whatever flash holds now is what a reboot will see
    }
    // Whatever flash did, the count in RAM never goes down here: what flash holds short of
    // `want` (a restart that failed after its erase lost the carry) is made up in RAM.
    unsigned flash_count = attempts - cleared;
    extra = want > flash_count ? want - flash_count : 0;
    return ok;
}

bool failcount_clear(void) {
    extra = 0;
    if (!have_marker || cleared == attempts)
        return true;
    if (!clear_bits(CLEARED_OFFSET, cleared, attempts)) {
        load();                         // whatever did get written is what a reboot will see
        return false;
    }
    cleared = attempts;
    return true;
}
