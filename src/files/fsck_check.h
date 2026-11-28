// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

// The `sd-check` health check (docs/design/health.md §9.7; core/health_board.c lists it): the
// judging here (pure, host tested), the read step in files/fsck_check_board.c.

#ifndef SOS_FSCK_CHECK_H
#define SOS_FSCK_CHECK_H

#include <stdbool.h>
#include <stdint.h>

#include "core/health.h"
#include "files/fsck.h"

// --- sd-check ------------------------------------------------------------------------------------

typedef enum {
    FSCK_CHECK_DONE,                  // fsck finished: res
    FSCK_CHECK_FAILED,                // fsck stopped or couldn't start: res->why, res->passes
    FSCK_CHECK_MOUNTED,               // the card is mounted: not run
    FSCK_CHECK_NOT_BY_HAND,           // not mounted, but not unmounted by hand either
    FSCK_CHECK_BUSY,                  // `sd check` or a format owns the card: why
    FSCK_CHECK_NOT_OPENED,            // the card couldn't be opened or no memory: why
} fsck_check_state_t;

typedef struct {
    fsck_check_state_t state;
    const fsck_result_t *res;           // DONE, FAILED
    bool encrypted;
    const char *why;                    // BUSY, NOT_OPENED
    uint32_t ms;
} fsck_check_in_t;

// PASS no problems; FAIL cross-linked clusters or (exFAT) clusters reached but free in the bitmap,
// since the next write can overwrite a file; WARN other problems, or a check that stopped or
// found no volume; SKIP when it didn't run, saying how to run it.
void fsck_check_judge(const fsck_check_in_t *in, health_result_t *out);

health_step_t fsck_check_step(void **state, bool cancel, health_result_t *out);

#endif
