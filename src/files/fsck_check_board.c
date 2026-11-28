// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The `sd-check` health check's read step (docs/design/health.md §9.7), moved from
// core/fullcheck_board.c (docs/design/build-features.md §4.3): `sd check`'s passes over an
// unmounted card, judged by files/fsck_check.c.

#include "files/fsck_check.h"

#include <stdio.h>

#include "pico/stdlib.h"

#include "core/heap.h"
#include "core/heaptag.h"
#include "files/sdcard.h"

// --- sd-check ------------------------------------------------------------------------------------

typedef struct {
    fsck_result_t res;
    bool encrypted;
    uint64_t t0;
} sd_state_t;

health_step_t fsck_check_step(void **state, bool cancel, health_result_t *out) {
    sd_state_t *st = *state;
    if (cancel) {
        if (st) {
            fsck_result_t res;
            bool enc;
            sdcard_check_health_step(true, &res, &enc);
            heap_release(HEAP_HEALTH, st);
        }
        *state = NULL;
        return HEALTH_DONE;
    }
    if (!st) {
        st = heap_zalloc(HEAP_HEALTH, sizeof(*st));
        if (!st) {
            out->status = HEALTH_SKIP;
            snprintf(out->text, sizeof(out->text), "no memory to check the card");
            return HEALTH_DONE;
        }
        st->t0 = time_us_64();
        char why[FSCK_LINE_MAX];
        // Opens the card as a mount does: the one step allowed to take longer than 10 ms.
        sdcard_check_start_t s = sdcard_check_health_begin(why, sizeof(why));
        if (s != SDCARD_CHECK_STARTED) {
            fsck_check_in_t in = {
                .state = s == SDCARD_CHECK_MOUNTED      ? FSCK_CHECK_MOUNTED
                         : s == SDCARD_CHECK_NOT_BY_HAND ? FSCK_CHECK_NOT_BY_HAND
                         : s == SDCARD_CHECK_BUSY        ? FSCK_CHECK_BUSY
                                                         : FSCK_CHECK_NOT_OPENED,
                .why = why,
            };
            fsck_check_judge(&in, out);
            heap_release(HEAP_HEALTH, st);
            return HEALTH_DONE;
        }
        *state = st;
        return HEALTH_MORE;
    }
    int r = sdcard_check_health_step(false, &st->res, &st->encrypted);
    if (r == FSCK_MORE)
        return HEALTH_MORE;
    fsck_check_in_t in = {
        .state = r == FSCK_DONE ? FSCK_CHECK_DONE : FSCK_CHECK_FAILED,
        .res = &st->res, .encrypted = st->encrypted,
        .ms = (uint32_t)((time_us_64() - st->t0) / 1000),
    };
    if (r == SDCARD_CHECK_ENDED) {      // nothing was running: shouldn't happen
        in.state = FSCK_CHECK_NOT_OPENED;
        in.why = "the check ended early";
    }
    fsck_check_judge(&in, out);
    heap_release(HEAP_HEALTH, st);
    *state = NULL;
    return HEALTH_DONE;
}
