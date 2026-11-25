// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Sealing stored records: AES-256-GCM under a key from the key provider (core/keyprov.h), a
// random 96-bit nonce for every seal, and the record's header as associated data. A sealed
// record read off the flash chip is unreadable without the key, and one that was changed, or
// written by someone without the key, doesn't open.
//
// Nonces are random, not counted: a counter kept in flash could be rolled back by rewriting the
// flash. With 96 random bits, the chance that two of the seals a board ever makes share a nonce
// stays negligible (a board saves its configuration a few thousand times at most; the usual
// limit for random nonces is 2^32 seals per key).
//
// Pure C over wolfCrypt, host tested. Randomness comes through a function pointer.

#ifndef SOS_SEAL_H
#define SOS_SEAL_H

#include "core/sealer.h"

#define SEAL_KEY_LEN 32

typedef struct {
    uint8_t key[SEAL_KEY_LEN];
    bool have_key;              // false: sealing and opening fail
    // Fill out with len random bytes. True on success.
    bool (*random)(void *ctx, uint8_t *out, size_t len);
    void *random_ctx;
} seal_ctx_t;

// The sealer_t functions; their ctx is a seal_ctx_t.
bool seal_seal(void *ctx, const uint8_t *aad, size_t aad_len, const uint8_t *plain, size_t len,
               uint8_t *out);
bool seal_open(void *ctx, const uint8_t *aad, size_t aad_len, const uint8_t *sealed,
               size_t sealed_len, uint8_t *plain);

// A sealer over `ctx`.
static inline sealer_t seal_sealer(seal_ctx_t *ctx) {
    sealer_t s = { seal_seal, seal_open, ctx };
    return s;
}

// Forget the key.
void seal_wipe(seal_ctx_t *ctx);

#endif
