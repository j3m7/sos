// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See sealedonly.h. The decision; the board's part is in sealedonly_board.c.

#include "core/sealedonly.h"

#include "core/bootkey.h"

sealedonly_t sealedonly_decide(const uint32_t rows[SEALEDONLY_COPIES]) {
    bool blank = true;
    for (unsigned i = 0; i < SEALEDONLY_COPIES; i++) {
        if (rows[i] == 0xffffffffu)     // a raw row has 24 bits: this is "not readable"
            return SEALEDONLY_YES_UNREADABLE;
        if (rows[i] & 0xffffffu)
            blank = false;
    }
    if (blank)
        return SEALEDONLY_NO;
    // Not blank: it is sealed-only whatever is there. The vote only tells a clean marker from
    // a damaged one, for the report.
    return (bootkey_vote(rows, SEALEDONLY_COPIES, 2) & 0xffffffu) == SEALEDONLY_PATTERN
               ? SEALEDONLY_YES
               : SEALEDONLY_YES_DAMAGED;
}
