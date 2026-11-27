// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See consoleonly.h: reading the marker on the board. Reads only (core/otpread.h).

#include "core/consoleonly.h"

#include "core/log.h"
#include "core/otpread.h"

static uint32_t rows[CONSOLEONLY_COPIES];
// Until the rows have been read the answer is "yes": nothing may load an unsealed record on
// the strength of a question that wasn't asked.
static consoleonly_t state = CONSOLEONLY_YES_UNREADABLE;
static bool forced;

#ifndef SOS_FORCE_CONSOLE_ONLY
#define SOS_FORCE_CONSOLE_ONLY 0
#endif

void consoleonly_init(void) {
    for (unsigned i = 0; i < CONSOLEONLY_COPIES; i++)
        rows[i] = otpread_raw(CONSOLEONLY_ROW + i);
    state = consoleonly_decide(rows);
    if (SOS_FORCE_CONSOLE_ONLY && state == CONSOLEONLY_NO) {
        forced = true;
        state = CONSOLEONLY_YES;
        log_msg(SOS_LOG_KERN | SOS_LOG_WARNING, "console-only setup: FORCED BY THIS BUILD "
                                 "(SOS_FORCE_CONSOLE_ONLY) for testing; the board's OTP "
                                 "marker is blank. Unsealed "
                                 "configuration and host key records are refused");
        return;
    }
    switch (state) {
    case CONSOLEONLY_NO:
        break;                          // an open board: nothing to say at every boot
    case CONSOLEONLY_YES:
        log_msg(SOS_LOG_KERN | SOS_LOG_NOTICE,
                "console-only board: unsealed configuration and host key records "
                                "are refused; set up on the console only");
        break;
    case CONSOLEONLY_YES_UNREADABLE:
        log_msg(SOS_LOG_KERN | SOS_LOG_ERR,
                "console-only setup: the marker's OTP rows can't be read; "
                             "treated as set, so unsealed records are refused");
        break;
    case CONSOLEONLY_YES_DAMAGED:
        log_msg(SOS_LOG_KERN | SOS_LOG_ERR, "console-only setup: the marker's OTP rows hold "
                             "0x%06lx 0x%06lx 0x%06lx, neither blank nor the marker; "
                             "treated as set, so unsealed records are refused",
                (unsigned long)rows[0], (unsigned long)rows[1], (unsigned long)rows[2]);
        break;
    }
}

consoleonly_t consoleonly_state(void) { return state; }
bool consoleonly(void) { return state != CONSOLEONLY_NO; }
const uint32_t *consoleonly_rows(void) { return rows; }
bool consoleonly_forced(void) { return forced; }
