// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See sealedonly.h: reading the marker on the board. Reads only (core/otpread.h).

#include "core/sealedonly.h"

#include "core/log.h"
#include "core/otpread.h"

static uint32_t rows[SEALEDONLY_COPIES];
// Until the rows have been read the answer is "yes": nothing may load an unsealed record on
// the strength of a question that wasn't asked.
static sealedonly_t state = SEALEDONLY_YES_UNREADABLE;
static bool forced;

#ifndef SOS_FORCE_SEALED_ONLY
#define SOS_FORCE_SEALED_ONLY 0
#endif

void sealedonly_init(void) {
    for (unsigned i = 0; i < SEALEDONLY_COPIES; i++)
        rows[i] = otpread_raw(SEALEDONLY_ROW + i);
    state = sealedonly_decide(rows);
    if (SOS_FORCE_SEALED_ONLY && state == SEALEDONLY_NO) {
        forced = true;
        state = SEALEDONLY_YES;
        log_msg(SOS_LOG_WARNING, "sealed-only: FORCED BY THIS BUILD (SOS_FORCE_SEALED_ONLY) for "
                                 "testing; the board's OTP marker is blank. Unsealed "
                                 "configuration and host key records are refused");
        return;
    }
    switch (state) {
    case SEALEDONLY_NO:
        break;                          // an open board: nothing to say at every boot
    case SEALEDONLY_YES:
        log_msg(SOS_LOG_NOTICE, "sealed-only board: unsealed configuration and host key records "
                                "are refused; set up on the console only");
        break;
    case SEALEDONLY_YES_UNREADABLE:
        log_msg(SOS_LOG_ERR, "sealed-only: the marker's OTP rows can't be read; treated as set, "
                             "so unsealed records are refused");
        break;
    case SEALEDONLY_YES_DAMAGED:
        log_msg(SOS_LOG_ERR, "sealed-only: the marker's OTP rows hold 0x%06lx 0x%06lx 0x%06lx, "
                             "neither blank nor the marker; treated as set, so unsealed records "
                             "are refused",
                (unsigned long)rows[0], (unsigned long)rows[1], (unsigned long)rows[2]);
        break;
    }
}

sealedonly_t sealedonly_state(void) { return state; }
bool sealedonly(void) { return state != SEALEDONLY_NO; }
const uint32_t *sealedonly_rows(void) { return rows; }
bool sealedonly_forced(void) { return forced; }
