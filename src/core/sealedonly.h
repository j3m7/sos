// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// "Sealed-only": a board that refuses unsealed records (issue #64, docs/securing-a-board.md).
//
// An open board loads an unsealed configuration or SSH host key from flash and seals it: that
// is how `ninja provision` and firmware from before sealing hand over. On a board meant to keep
// its secrets that would be a way in, since anyone with a cable can write the flash: write an
// unsealed configuration naming your own SSH key, and the board lets you in. A sealed-only
// board ignores unsealed records and can be set up only on its console. It also refuses
// `rootsecret forget`.
//
// What makes a board sealed-only is a marker of its own, not secure boot: a development board
// can have secure boot and still be provisioned from a file. The marker is in one-time-
// programmable memory, so rewriting the flash can't remove it, and it is the same for every
// firmware image the board will ever run:
//
//   OTP rows 0x0c0, 0x0c1 and 0x0c2 (the first three of page 3, the first page the chip
//   leaves to the user), each holding the raw 24-bit value 0x5ea1ed, read by a two-of-three
//   vote.
//
// It fails closed. Blank rows mean "no". The pattern means "yes". Anything else also means
// "yes": rows that can't be read, or that hold something that is neither blank nor the pattern
// (a write that was cut short, or someone setting more bits; OTP bits can be set but never
// cleared, so the marker can be damaged but not removed).
//
// This firmware only reads the rows (core/otpread.h). Writing them is a step of the procedure
// for securing a production board, done from outside with picotool by the board's owner.
//
// sealedonly_decide() has no hardware in it and is unit tested on the host.

#ifndef SOS_SEALEDONLY_H
#define SOS_SEALEDONLY_H

#include <stdbool.h>
#include <stdint.h>

#define SEALEDONLY_ROW     0x0c0u       // the first of the three rows
#define SEALEDONLY_COPIES  3
#define SEALEDONLY_PATTERN 0x5ea1edu

typedef enum {
    SEALEDONLY_NO,                      // blank: an open board, unsealed records are loaded
    SEALEDONLY_YES,                     // the marker is there
    SEALEDONLY_YES_UNREADABLE,          // a row can't be read: treated as there
    SEALEDONLY_YES_DAMAGED,             // neither blank nor the marker: treated as there
} sealedonly_t;

// From the three raw rows as the chip's raw read alias returns them (24 bits, or 0xffffffff
// for a row that can't be read).
sealedonly_t sealedonly_decide(const uint32_t rows[SEALEDONLY_COPIES]);

// On the board. sealedonly_init() reads the rows once, early in boot, before anything loads a
// record; the answer doesn't change while the board runs.
void sealedonly_init(void);
sealedonly_t sealedonly_state(void);
bool sealedonly(void);                  // anything but SEALEDONLY_NO
// The three rows as read at boot, for `version`.
const uint32_t *sealedonly_rows(void);
// The board's marker is blank and this build treats the board as sealed-only all the same
// (SOS_FORCE_SEALED_ONLY, a build option for trying the behaviour on a development board
// without writing its OTP). It can only add the restriction, never lift it.
bool sealedonly_forced(void);

#endif
