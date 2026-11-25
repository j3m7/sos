// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_RANDOM_H
#define SOS_RANDOM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Fill out with len bytes from wolfCrypt's random number generator (seeded from the RP2350's
// TRNG). True on success. `ctx` is unused: the signature is the one the pure-C modules take
// their randomness through (core/seal.h, core/rootsecret.h). After wolfCrypt_Init().
bool random_bytes(void *ctx, uint8_t *out, size_t len);

#endif
