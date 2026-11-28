// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The pure half of phase 5's health checks, the full tier (core/health.h,
// docs/design/health.md §9.7): `ram`, `image` and `sd-check`, run only by `health full` or
// `health run <name>`. fullcheck_board.c does the work on the board (the march test of
// core/memtest.h, the image's hash, `sd check`'s job) and hands what it found here;
// tests/test_fullcheck.c runs these with made-up inputs, one test per status path at least.
//
// `ram` and `image` hold KEYS and CRYPTO (health_board.c), closed by their FAILs only; a WARN
// holds nothing. `sd-check` holds no gate. None repairs anything.

#ifndef SOS_FULLCHECK_H
#define SOS_FULLCHECK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/bootkey.h"
#include "core/health.h"

// --- ram -----------------------------------------------------------------------------------------

#define FULLCHECK_RAM_BLOCK   4096u     // bytes of heap tested a step
#define FULLCHECK_RAM_PIECE   1024u     // bytes of SCRATCH_Y tested a step, in place
#define FULLCHECK_RAM_RESERVE HEALTH_HEAP_CLEAR     // free heap the test never takes

typedef enum {
    FULLCHECK_RAM_CLEAN,                // every read gave what was written
    FULLCHECK_RAM_ONCE,                 // a mismatch the second test didn't repeat
    FULLCHECK_RAM_TWICE,                // the same address wrong in both tests
} fullcheck_ram_fault_t;

typedef struct {
    size_t heap_bytes;                  // free heap tested
    size_t free_before;                 // heap_free() when the test began
    bool scratch_tested;                // SCRATCH_Y's 4 KB
    const char *scratch_why;            // not tested: "SCRATCH_Y in use", "DMA 3 writes
                                        // SCRATCH_Y", ...; NULL when tested
    uint32_t ms;                        // the test's time
    fullcheck_ram_fault_t fault;
    uint32_t addr, wrote, read;         // the (first) mismatch
} fullcheck_ram_t;

// FAIL a mismatch that repeated at the same address; WARN one that didn't; PASS otherwise when
// anything was tested; SKIP when the free heap was under the reserve and SCRATCH_Y not testable.
void fullcheck_judge_ram(const fullcheck_ram_t *in, health_result_t *out);

// --- image ---------------------------------------------------------------------------------------

typedef struct {
    bootkey_hash_kind_t boot;           // the image's hash items as found at boot
    bootkey_hash_kind_t now;            // and as found in flash now
    const bootkey_hash_t *at_boot;      // the HASH_VALUE copied at boot (boot == OK)
    const bootkey_hash_t *in_flash;     // the items in flash now (now == OK)
    const uint8_t *computed;            // the SHA-256 the check computed (32 bytes); NULL if none
    size_t bytes;                       // bytes hashed
    uint32_t ms;
} fullcheck_image_t;

// In this order: SKIP no hash at boot and none now; WARN items not understood (at boot or now,
// the same both times); FAIL the hash items changed since boot (judged before the body: a
// different finding); FAIL the computed hash differs from the copy taken at boot; PASS.
void fullcheck_judge_image(const fullcheck_image_t *in, health_result_t *out);

// What the hash covers, a piece at a time: each load map entry's stored bytes (or, for a
// zero-fill entry, its size as one little-endian word), then the block's own words. A piece is
// at most `max` bytes. False when there is nothing left.
typedef struct {
    unsigned entry;                     // the load map entry; h->loads: the block's words
    uint32_t done;                      // bytes of it already given
} fullcheck_hash_cursor_t;

typedef struct {
    uint32_t off, len;                  // bytes from the image's start
    bool size_word;                     // a zero-fill entry: hash `size` as a word instead
    uint32_t size;
} fullcheck_piece_t;

bool fullcheck_hash_next(const bootkey_hash_t *h, fullcheck_hash_cursor_t *c, uint32_t max,
                         fullcheck_piece_t *p);
size_t fullcheck_hash_bytes(const bootkey_hash_t *h);

// Does the computed SHA-256 match the HASH_VALUE (its first value_words words, little-endian)?
bool fullcheck_hash_matches(const bootkey_hash_t *h, const uint8_t computed[32]);

#endif
