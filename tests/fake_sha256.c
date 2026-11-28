// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See fake_sha256.h.

#include "fake_sha256.h"

#include <string.h>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/sha256.h>

#include "pico/sha256.h"

fake_sha256_t fake_sha256;
static wc_Sha256 sha;

void fake_sha256_reset(void) {
    if (fake_sha256.locked)
        wc_Sha256Free(&sha);
    memset(&fake_sha256, 0, sizeof(fake_sha256));
}

int pico_sha256_try_start(pico_sha256_state_t *state, enum sha256_endianness endianness,
                          bool use_dma) {
    memset(state, 0, sizeof(*state));           // as the SDK does, before taking the lock
    if (fake_sha256.taken || fake_sha256.locked) {
        fake_sha256.refused++;
        return PICO_ERROR_RESOURCE_IN_USE;
    }
    if (wc_InitSha256(&sha) != 0)
        return PICO_ERROR_RESOURCE_IN_USE;
    state->endianness = endianness;
    state->locked = true;
    fake_sha256.locked = true;
    fake_sha256.dma = fake_sha256.dma || use_dma;
    fake_sha256.starts++;
    fake_sha256.bytes = 0;
    return PICO_OK;
}

void pico_sha256_update_blocking(pico_sha256_state_t *state, const uint8_t *data,
                                 size_t data_size_bytes) {
    if (!state->locked || !fake_sha256.locked)
        return;
    wc_Sha256Update(&sha, data, (word32)data_size_bytes);
    fake_sha256.bytes += data_size_bytes;
}

void pico_sha256_finish(pico_sha256_state_t *state, sha256_result_t *out) {
    if (!state->locked || !fake_sha256.locked)
        return;
    if (out) {
        wc_Sha256Final(&sha, out->bytes);
        if (state->endianness != SHA256_BIG_ENDIAN)
            memset(out->bytes, 0, sizeof(out->bytes));  // not the order SOS reads
        if (fake_sha256.wrong)
            out->bytes[5] ^= 0x10;
        fake_sha256.finished++;
    } else {
        fake_sha256.abandoned++;
    }
    wc_Sha256Free(&sha);
    state->locked = false;
    fake_sha256.locked = false;
}
