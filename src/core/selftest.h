// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Known-answer tests of the crypto SOS relies on: the `crypto` health check (core/health.h,
// docs/design/health.md §10). Pure C over wolfCrypt, so the host tests run the same vectors
// against the host's wolfCrypt (tests/test_selftest.c).
//
// Each test computes a published test vector and compares the result byte for byte. Where SOS
// relies on a refusal (a GCM tag that doesn't match, an Ed25519 signature over another message),
// the test also feeds a copy with one bit flipped and expects the refusal: a primitive stuck at
// "OK" would pass a test of the good input alone.

#ifndef SOS_SELFTEST_H
#define SOS_SELFTEST_H

#include <stdbool.h>

// Run every test, in order, stopping at the first that fails. NULL if all passed, else what
// failed, as a short phrase for the check's text ("ed25519 verify accepted a changed message").
// *ran: the number of tests run. Needs wolfCrypt_Init() first; takes about 1.7 KB of heap
// (HEAP_NET) for the cipher contexts while it runs.
const char *selftest_crypto(unsigned *ran);

// The number of tests selftest_crypto() runs when all pass.
unsigned selftest_crypto_count(void);

#ifdef SELFTEST_TEST
// Host tests only: make test `index` (0-based, in run order) see a fault. A comparison sees one
// bit of the computed output flipped; a refusal test is fed the unchanged input, as if the
// primitive accepted the changed one. -1: no fault.
void selftest_crypto_fault(int index);
#endif

#endif
