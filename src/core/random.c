// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "core/random.h"

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/random.h>

bool random_bytes(void *ctx, uint8_t *out, size_t len) {
    (void)ctx;
    WC_RNG rng;
    if (wc_InitRng(&rng) != 0)
        return false;
    int rc = wc_RNG_GenerateBlock(&rng, out, (word32)len);
    wc_FreeRng(&rng);
    return rc == 0;
}
