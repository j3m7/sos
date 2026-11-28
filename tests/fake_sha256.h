// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// A fake of the SDK's pico_sha256 (tests/stubs/pico/sha256.h) over wolfCrypt's SHA-256, so the
// host tests run src/files/sha256hw.c and its callers on "the block". The lock behaves as the
// boot lock: one holder; fake_sha256.taken stands for someone else holding it.

#ifndef SOS_FAKE_SHA256_H
#define SOS_FAKE_SHA256_H

#include <stdbool.h>

typedef struct {
    bool taken;         // someone else (another shell, the bootrom) holds the lock
    bool locked;        // a hash started here holds it
    bool wrong;         // every result has a bit flipped: a broken block
    bool dma;           // a start asked for DMA
    int starts;         // successful starts
    int refused;        // starts refused because the lock was held
    int finished;       // finishes with a result
    int abandoned;      // finishes with NULL
    unsigned long bytes;    // bytes hashed since the last start
} fake_sha256_t;

extern fake_sha256_t fake_sha256;

// Everything back to the start, the lock free.
void fake_sha256_reset(void);

#endif
