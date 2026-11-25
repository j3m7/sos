// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The root secret's record on an open board: 32 random bytes from which every other key is
// derived (core/keyprov.h, issue #64). On a board without secure boot it's kept in flash, in
// a sector of its own (core/rootflash.c). That protects nothing: anyone with the board can read
// the flash. It's there so the same formats and code run on an open board as on a secured one,
// where the secret is in OTP memory instead.
//
// The record (one 256-byte flash page; the rest of the page and of the sector stays erased):
//    0  magic "SOSR"                4   (0x52534f53 little-endian)
//    4  version (1)                 4
//    8  secret                      32
//   40  check: FNV-1a over secret   4   (catches a damaged record; not a security check)
//
// Rules:
// - It's made once, from the random number generator, when the page is blank (all 0xff).
// - A page that isn't blank and isn't a valid record is never replaced automatically: it may be
//   a record from newer firmware, and replacing it would silently make a board that can't open
//   its own configuration or cards. Only rootsecret_forget(), after the user confirmed, clears it.
//
// Pure C: storage and randomness come through rootsecret_store_t, so it's unit tested on the
// host.

#ifndef SOS_ROOTSECRET_H
#define SOS_ROOTSECRET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ROOTSECRET_LEN  32
#define ROOTSECRET_PAGE 256             // one flash page

enum {
    ROOTSECRET_NONE,        // blank page: rootsecret_ensure() makes one
    ROOTSECRET_OK,
    ROOTSECRET_INVALID,     // something this firmware can't use: left alone
};

typedef struct {
    // The page as stored (ROOTSECRET_PAGE bytes; erased flash reads as 0xff).
    const uint8_t *(*read)(void *ctx);
    // Replace the page (NULL erases it). True on success.
    bool (*write)(void *ctx, const uint8_t *page);
    // Fill out with len random bytes. True on success.
    bool (*random)(void *ctx, uint8_t *out, size_t len);
    void *ctx;
} rootsecret_store_t;

int rootsecret_state(const rootsecret_store_t *store);

// Make and store a secret if there's none (ROOTSECRET_NONE). Returns the state afterwards:
// ROOTSECRET_OK, ROOTSECRET_INVALID (left alone), or ROOTSECRET_NONE if making or storing one
// failed. *created, if not NULL, says whether this call made it.
int rootsecret_ensure(const rootsecret_store_t *store, bool *created);

// Copy the secret out. False (and `out` zeroed) unless the state is ROOTSECRET_OK. The caller
// wipes `out` after use.
bool rootsecret_read(const rootsecret_store_t *store, uint8_t out[ROOTSECRET_LEN]);

// Erase the page: the next rootsecret_ensure() makes a new secret. Everything derived from the
// old one is lost.
bool rootsecret_forget(const rootsecret_store_t *store);

#endif
