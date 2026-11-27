// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Known-answer tests of the crypto SOS relies on: the `crypto` and `crypto-ssh` health checks
// (core/health.h, docs/design/health.md §9.2). Pure C over wolfCrypt, so the host tests run the
// same vectors against the host's wolfCrypt, built with the firmware's options
// (tests/test_selftest.c, tests/wolfcrypt_host/user_settings.h).
//
// Each test computes a published test vector and compares the result byte for byte. Where SOS
// relies on a refusal (a GCM tag that doesn't match, an Ed25519 signature over another message),
// the test also feeds a copy with one bit flipped and expects the refusal: a primitive stuck at
// "OK" would pass a test of the good input alone.
//
// The tests run one at a time (selftest_run), so a caller can spread them over several steps
// and time each. The boot subset (selftest_info()->boot) covers what making keys and sealing at
// boot rely on and fits the boot tier's budget; the rest (Ed25519 signing and verifying, X25519:
// the SSH primitives, tens of milliseconds each) run after boot.

#ifndef SOS_SELFTEST_H
#define SOS_SELFTEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The groups the text reports times by.
typedef enum {
    SELFTEST_SHA,           // SHA-256, SHA-512, HMAC-SHA-256
    SELFTEST_GCM,
    SELFTEST_XTS,
    SELFTEST_ED_PUB,        // the Ed25519 public key from its secret (as a host key is made)
    SELFTEST_ED_SIGN,
    SELFTEST_ED_VERIFY,     // a good signature, and a changed message refused
    SELFTEST_X25519,
    SELFTEST_GROUPS
} selftest_group_t;

typedef struct {
    const char *name;       // "gcm-refuse": for `health crypto-times`
    selftest_group_t group;
    bool boot;              // in the boot subset
} selftest_info_t;

unsigned selftest_count(void);
const selftest_info_t *selftest_info(unsigned index);  // NULL past the end
const char *selftest_group_name(selftest_group_t g);    // "sha", "ed-verify"

// The contexts the tests use (about 2 KB of heap, HEAP_NET, while open). NULL: no memory.
// Needs wolfCrypt_Init() first.
typedef struct selftest selftest_t;
selftest_t *selftest_open(void);
void selftest_close(selftest_t *st);    // NULL is fine

// Run test `index`: NULL if it passed, else what failed, as a phrase for the check's text. A
// wrong result and an error from wolfCrypt read differently: "x25519 gave a wrong shared
// secret", "x25519 shared secret failed: error -173".
const char *selftest_run(selftest_t *st, unsigned index);

// The check's text from the times per group (microseconds; a group with no test run is
// left out): "9 known answers in 23 ms: sha 0.4 gcm 1.2 xts 0.3 ed-pub 19". Cut to fit cap.
void selftest_text(char *out, size_t cap, unsigned ran, uint32_t total_us,
                   const uint32_t group_us[SELFTEST_GROUPS],
                   const unsigned group_runs[SELFTEST_GROUPS]);

#ifdef SELFTEST_TEST
// Host tests only: make test `index` see a fault. A comparison sees one bit of the computed
// output flipped; a refusal test is fed the unchanged input, as if the primitive accepted the
// changed one. -1: no fault.
void selftest_fault(int index);
#endif

#endif
