// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See rootburn.h. Compiled only into the burn image.

#include "core/rootburn.h"

#include <string.h>

static void wipe(void *p, size_t len) {
    volatile uint8_t *v = p;
    while (len--)
        *v++ = 0;
}

static void tick(const rootburn_ops_t *ops) {
    if (ops->tick)
        ops->tick(ops->ctx);
}

// Does the list's page `index` read as never written (every row zero)?
static bool reads_blank(const rootburn_ops_t *ops, unsigned index) {
    const roototp_reader_t *r = &ops->reader;
    for (unsigned row = 0; row < ROOTOTP_PAGE_ROWS; row++)
        if (r->raw(r->ctx, ROOTOTP_ROW(index, row)) != 0)
            return false;               // something landed, or it can't be read
    return true;
}

// Lock a page and see that it took. The write carries the bits the lock word already holds:
// the boot ROM refuses a write that would leave a set bit unset.
static bool lock_page(const rootburn_ops_t *ops, unsigned page) {
    const roototp_reader_t *r = &ops->reader;
    uint32_t have = r->lock1(r->ctx, page);
    if (have == 0xffffffffu)
        return false;
    tick(ops);
    if (!ops->write_lock1(ops->ctx, page, (have | ROOTBURN_LOCK_WORD) & 0xffffffu))
        return false;
    uint32_t now = r->lock1(r->ctx, page);
    return now != 0xffffffffu && (now & ROOTBURN_LOCK_WORD) == ROOTBURN_LOCK_WORD &&
           !roototp_bootloader_can_write(now);
}

// Lock every page of the list from the first up to and including `last` that holds something
// and that the bootloader can still write. A page that reads blank is NEVER locked: a locked
// blank page can be neither written nor passed over. Returns false, with *failed the page, if
// a lock didn't take; *did says whether any lock was written.
static bool lock_written_pages(const rootburn_ops_t *ops, unsigned last, bool *did,
                               unsigned *failed) {
    const roototp_reader_t *r = &ops->reader;
    for (unsigned i = 0; i <= last && i < ROOTOTP_PAGES; i++) {
        unsigned page = ROOTOTP_FIRST_PAGE + i;
        if (!roototp_bootloader_can_write(r->lock1(r->ctx, page)) || reads_blank(ops, i))
            continue;
        *did = true;
        if (!lock_page(ops, page)) {
            *failed = page;
            return false;
        }
    }
    return true;
}

// Write one error-corrected row and read it back.
static bool write_row(const rootburn_ops_t *ops, unsigned row, uint16_t value) {
    const roototp_reader_t *r = &ops->reader;
    uint16_t back = (uint16_t)~value;
    tick(ops);
    return ops->write_ecc(ops->ctx, row, value) && r->careful(r->ctx, row, 1, &back) &&
           back == value;
}

void rootburn_run(const rootburn_ops_t *ops, bool rehearse, rootburn_report_t *rep) {
    const roototp_reader_t *r = &ops->reader;
    uint8_t secret[ROOTOTP_SECRET_LEN], check[ROOTOTP_CHECK_LEN], back[ROOTOTP_SECRET_LEN];
    unsigned index = 0, failed = 0;
    memset(rep, 0, sizeof(*rep));

    // Nothing is written before these have all passed.
    if (!ops->present(ops->ctx)) {
        rep->result = ROOTBURN_NOT_PRESENT;
        return;
    }
    switch (roototp_find(r, &index)) {
    case ROOTOTP_NONE_YET:
        break;
    case ROOTOTP_FOUND:
        // The board has its secret. The one thing left to do, if an earlier run couldn't, is
        // to lock its page and the spent ones before it.
        rep->page = ROOTOTP_FIRST_PAGE + index;
        if (lock_written_pages(ops, index, &rep->wrote, &failed)) {
            rep->result = rep->wrote ? ROOTBURN_RELOCKED : ROOTBURN_HAS_SECRET;
            rep->page_locked = true;
        } else {
            rep->result = ROOTBURN_LOCK;
            rep->page = failed;
        }
        return;
    default:
        rep->result = ROOTBURN_NO_PAGE;
        return;
    }
    unsigned page = ROOTOTP_FIRST_PAGE + index;
    rep->page = page;

    // 0. The spent pages before this one.
    if (index > 0 && !lock_written_pages(ops, index - 1, &rep->wrote, &failed)) {
        rep->result = ROOTBURN_EARLIER_LOCK;
        rep->page = failed;
        return;
    }

    // 1. The secret. A generator that returns the same byte 32 times is broken, whatever its
    // own tests said; and 32 zero bytes would be written as a secret all the same.
    bool varied = false;
    if (ops->random(ops->ctx, secret, sizeof(secret)))
        for (unsigned i = 1; i < sizeof(secret); i++)
            if (secret[i] != secret[0])
                varied = true;
    if (!varied || !roototp_check(secret, check)) {
        rep->result = ROOTBURN_RANDOM;
        wipe(secret, sizeof(secret));
        return;
    }
    if (!ops->present(ops->ctx)) {      // still there, at the last moment before it is real
        rep->result = ROOTBURN_NOT_PRESENT;
        wipe(secret, sizeof(secret));
        return;
    }

    // From here the page is spent unless everything succeeds.
    rep->result = ROOTBURN_WRITE;

    // 2. The secret's rows.
    for (unsigned k = 0; k < ROOTOTP_SECRET_ROWS; k++) {
        rep->row = ROOTOTP_ROW(index, k);
        if (!write_row(ops, rep->row, (uint16_t)(secret[2 * k] | secret[2 * k + 1] << 8)))
            goto spent;
    }
    // 3. The chaff: the complement of what each row now holds, error-correction bits included.
    for (unsigned k = 0; k < ROOTOTP_SECRET_ROWS; k++) {
        uint32_t raw = r->raw(r->ctx, ROOTOTP_ROW(index, k));
        uint32_t chaff = ~raw & 0xffffffu;
        rep->row = ROOTOTP_ROW(index, ROOTOTP_CHAFF_ROW + k);
        tick(ops);
        if (raw == 0xffffffffu || !ops->write_raw(ops->ctx, rep->row, chaff) ||
            r->raw(r->ctx, rep->row) != chaff)
            goto spent;
    }
    if (rehearse) {
        rep->result = ROOTBURN_REHEARSED;
        goto spent;                     // on purpose: locked like any other spent page
    }
    // 4. The check.
    for (unsigned k = 0; k < ROOTOTP_CHECK_ROWS; k++) {
        rep->row = ROOTOTP_ROW(index, ROOTOTP_CHECK_ROW + k);
        if (!write_row(ops, rep->row, (uint16_t)(check[2 * k] | check[2 * k + 1] << 8)))
            goto spent;
    }
    // 5. All of it, read as the firmware will read it, against what was meant.
    rep->row = 0;
    if (!roototp_read(r, index, back) || memcmp(back, secret, sizeof(secret)) != 0) {
        rep->result = ROOTBURN_VERIFY;
        goto spent;
    }
    // 6. The marker.
    rep->result = ROOTBURN_WRITE;
    rep->row = ROOTOTP_ROW(index, ROOTOTP_MARKER_ROW);
    if (!write_row(ops, rep->row, ROOTOTP_MARKER))
        goto spent;                     // if its bits landed in part the page reads as damaged
    rep->row = 0;
    if (roototp_page(r, index) != ROOTOTP_COMPLETE) {
        rep->result = ROOTBURN_VERIFY;
        goto spent;
    }
    // 7. The page's locks.
    rep->wrote = true;
    rep->page_locked = lock_page(ops, page);
    rep->result = rep->page_locked ? ROOTBURN_OK : ROOTBURN_LOCK;
    goto done;

spent:
    // If nothing landed, the page is still blank and must stay unlocked: it is the page the
    // next run will use, and a locked blank page would block the whole list. Otherwise it
    // holds something and no usable secret: lock it, so that nobody can finish it.
    if (reads_blank(ops, index)) {
        rep->nothing_landed = true;
    } else {
        rep->wrote = true;
        rep->page_locked = lock_page(ops, page);
    }
done:
    wipe(secret, sizeof(secret));
    wipe(check, sizeof(check));
    wipe(back, sizeof(back));
}

bool rootburn_must_stop(rootburn_result_t result) {
    switch (result) {
    case ROOTBURN_OK:
    case ROOTBURN_REHEARSED:
    case ROOTBURN_RELOCKED:
    case ROOTBURN_NOT_PRESENT:
    case ROOTBURN_HAS_SECRET:
        return false;
    default:
        return true;
    }
}

const char *rootburn_result_text(rootburn_result_t result) {
    switch (result) {
    case ROOTBURN_OK:           return "the root secret is written, verified and its page locked";
    case ROOTBURN_REHEARSED:    return "rehearsal: stopped on purpose before the check and the "
                                       "marker; that page is now spent";
    case ROOTBURN_RELOCKED:     return "this board already had its root secret in OTP; a page "
                                       "that wasn't locked is locked now";
    case ROOTBURN_NOT_PRESENT:  return "the button wasn't held: no secret was written";
    case ROOTBURN_HAS_SECRET:   return "this board already has a root secret in OTP, locked: "
                                       "nothing was written";
    case ROOTBURN_NO_PAGE:      return "there is no blank page to use (all spent, or one can't be "
                                       "read or is damaged): nothing was written";
    case ROOTBURN_EARLIER_LOCK: return "locking an earlier, spent page failed: no secret was "
                                       "written";
    case ROOTBURN_RANDOM:       return "the random number generator failed or gave 32 equal "
                                       "bytes: nothing was written";
    case ROOTBURN_WRITE:        return "a write failed or didn't read back";
    case ROOTBURN_VERIFY:       return "what was written didn't verify: that page is spent";
    case ROOTBURN_LOCK:         return "LOCKING A PAGE FAILED: the bootloader can still write it";
    }
    return "unknown result";
}
