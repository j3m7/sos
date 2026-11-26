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

// Lock a page and see that it took: the lock word must then say the bootloader can't write.
static bool lock_page(const rootburn_ops_t *ops, unsigned page) {
    const roototp_reader_t *r = &ops->reader;
    if (!ops->write_lock1(ops->ctx, page, ROOTBURN_LOCK_WORD))
        return false;
    uint32_t now = r->lock1(r->ctx, page);
    return now != 0xffffffffu && (now & ROOTBURN_LOCK_WORD) == ROOTBURN_LOCK_WORD &&
           !roototp_bootloader_can_write(now);
}

// Write one error-corrected row and read it back the careful way.
static bool write_row(const rootburn_ops_t *ops, unsigned row, uint16_t value) {
    const roototp_reader_t *r = &ops->reader;
    uint16_t back = (uint16_t)~value;
    return ops->write_ecc(ops->ctx, row, value) && r->careful(r->ctx, row, 1, &back) &&
           back == value;
}

void rootburn_run(const rootburn_ops_t *ops, bool rehearse, rootburn_report_t *rep) {
    const roototp_reader_t *r = &ops->reader;
    uint8_t secret[ROOTOTP_SECRET_LEN], check[ROOTOTP_CHECK_LEN], back[ROOTOTP_SECRET_LEN];
    unsigned index = 0;
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
        rep->result = ROOTBURN_HAS_SECRET;
        rep->page = ROOTOTP_FIRST_PAGE + index;
        return;
    default:
        rep->result = ROOTBURN_NO_PAGE;
        return;
    }
    unsigned page = ROOTOTP_FIRST_PAGE + index;
    rep->page = page;

    // 0. The spent pages before this one.
    for (unsigned i = 0; i < index; i++) {
        unsigned earlier = ROOTOTP_FIRST_PAGE + i;
        if (!roototp_bootloader_can_write(r->lock1(r->ctx, earlier)))
            continue;                   // locked already
        rep->wrote = true;
        if (!lock_page(ops, earlier)) {
            rep->result = ROOTBURN_EARLIER_LOCK;
            rep->page = earlier;
            return;
        }
    }

    // 1. The secret.
    if (!ops->random(ops->ctx, secret, sizeof(secret)) || !roototp_check(secret, check)) {
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
    rep->wrote = true;
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
    rep->page_locked = lock_page(ops, page);
    rep->result = rep->page_locked ? ROOTBURN_OK : ROOTBURN_LOCK;
    goto done;

spent:
    // The page holds something and no usable secret. Lock it, so that nobody can finish it.
    rep->page_locked = lock_page(ops, page);
done:
    wipe(secret, sizeof(secret));
    wipe(check, sizeof(check));
    wipe(back, sizeof(back));
}

const char *rootburn_result_text(rootburn_result_t result) {
    switch (result) {
    case ROOTBURN_OK:           return "the root secret is written, verified and its page locked";
    case ROOTBURN_REHEARSED:    return "rehearsal: stopped on purpose before the check and the "
                                       "marker; that page is now spent";
    case ROOTBURN_NOT_PRESENT:  return "the button wasn't held: nothing was written";
    case ROOTBURN_HAS_SECRET:   return "this board already has a root secret in OTP: nothing was "
                                       "written";
    case ROOTBURN_NO_PAGE:      return "there is no blank page to use (all spent, or one can't be "
                                       "read or is damaged): nothing was written";
    case ROOTBURN_EARLIER_LOCK: return "locking an earlier, spent page failed: no secret was "
                                       "written";
    case ROOTBURN_RANDOM:       return "the random number generator failed: nothing was written";
    case ROOTBURN_WRITE:        return "a write failed or didn't read back: that page is spent";
    case ROOTBURN_VERIFY:       return "what was written didn't verify: that page is spent";
    case ROOTBURN_LOCK:         return "the root secret is written and verified, BUT LOCKING ITS "
                                       "PAGE FAILED";
    }
    return "unknown result";
}
