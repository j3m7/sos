// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The root secret's record on an open board: 32 random bytes from which every other key is
// derived (core/keyprov.h, issue #64). On a board without secure boot it's kept in flash, in
// a sector of its own (core/rootflash.c). That protects nothing: anyone with the board can read
// the flash. It's there so the same formats and code run on an open board as on a secured one,
// where the secret is in OTP memory instead.
//
// The sector holds the same record twice, in its first two 256-byte flash pages (copy A in page
// 0, copy B in page 1), so one damaged page doesn't lose everything derived from the secret.
// The record (the rest of its page stays erased):
//    0  magic "SOSR"                4   (0x52534f53 little-endian)
//    4  version (1)                 4
//    8  secret                      32
//   40  check: FNV-1a over secret   4   (catches a damaged record; not a security check)
//
// Rules:
// - Making it: when both pages are blank (all 0xff), a secret from the random number generator
//   is programmed into page 1, then page 0, without erasing anything: a blank page needs no
//   erase, and nothing else in the sector is touched.
// - Page 0 is written last, and a secret is only ever used once page 0 holds it. So a blank
//   page 0 means no secret from this sector was ever used, and an interrupted first write (page
//   1 partly or fully written, page 0 blank) is finished or started over at the next boot.
// - Using it: either valid copy will do. If the other page is blank it's filled in; if it's
//   damaged it's left alone and reported (rootsecret_degraded).
// - Anything else is never replaced automatically: page 0 not blank and no valid copy (damaged,
//   or a record from newer firmware), or two valid copies that differ. Replacing it would
//   silently make a board that can't open its own configuration or cards. Only
//   rootsecret_forget(), after the user confirmed, clears it. That erases the whole sector.
//
// Pure C: storage and randomness come through rootsecret_store_t, so it's unit tested on the
// host. Reading the secret out is not part of this interface: see rootsecret_priv.h.

#ifndef SOS_ROOTSECRET_H
#define SOS_ROOTSECRET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ROOTSECRET_LEN    32
#define ROOTSECRET_PAGE   256           // one flash page
#define ROOTSECRET_SECTOR 4096          // one flash sector

enum {
    ROOTSECRET_NONE,        // no secret yet (or an unfinished first write): rootsecret_ensure() makes it
    ROOTSECRET_OK,
    ROOTSECRET_INVALID,     // something this firmware can't use: left alone
};

typedef struct {
    // The sector as stored (ROOTSECRET_SECTOR bytes; erased flash reads as 0xff).
    const uint8_t *(*read)(void *ctx);
    // Program one page (0-based, ROOTSECRET_PAGE bytes) without erasing. Only ever called for a
    // page that reads blank. True on success.
    bool (*program)(void *ctx, unsigned page, const uint8_t *data);
    // Erase the whole sector. True on success.
    bool (*erase)(void *ctx);
    // Fill out with len random bytes. True on success.
    bool (*random)(void *ctx, uint8_t *out, size_t len);
    void *ctx;
} rootsecret_store_t;

int rootsecret_state(const rootsecret_store_t *store);

// True if the state is ROOTSECRET_OK but only one of the two copies is valid.
bool rootsecret_degraded(const rootsecret_store_t *store);

// At boot: make and store a secret if there's none, finish an interrupted first write, and
// fill in a blank second copy. Returns the state afterwards: ROOTSECRET_OK, ROOTSECRET_INVALID
// (left alone), or ROOTSECRET_NONE if making or storing one failed (the next boot tries again).
// *created, if not NULL, says whether this call made a new secret.
int rootsecret_ensure(const rootsecret_store_t *store, bool *created);

// Erase the sector: the next rootsecret_ensure() makes a new secret. Everything derived from
// the old one is lost.
bool rootsecret_forget(const rootsecret_store_t *store);

#endif
