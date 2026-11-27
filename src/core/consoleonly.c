// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See consoleonly.h. The decision; the board's part is in consoleonly_board.c.

#include "core/consoleonly.h"

#include "core/bootkey.h"

consoleonly_t consoleonly_decide(const uint32_t rows[CONSOLEONLY_COPIES]) {
    bool blank = true;
    for (unsigned i = 0; i < CONSOLEONLY_COPIES; i++) {
        if (rows[i] == 0xffffffffu)     // a raw row has 24 bits: this is "not readable"
            return CONSOLEONLY_YES_UNREADABLE;
        if (rows[i] & 0xffffffu)
            blank = false;
    }
    if (blank)
        return CONSOLEONLY_NO;
    // Not blank: it is console-only whatever is there. The vote only tells a clean marker from
    // a damaged one, for the report.
    return (bootkey_vote(rows, CONSOLEONLY_COPIES, 2) & 0xffffffu) == CONSOLEONLY_PATTERN
               ? CONSOLEONLY_YES
               : CONSOLEONLY_YES_DAMAGED;
}
