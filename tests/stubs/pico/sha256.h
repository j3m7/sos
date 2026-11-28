// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Host-test stand-in for the Pico SDK's pico/sha256.h: what src/files/sha256hw.c uses, with
// the SDK's names and meanings. tests/fake_sha256.c implements it with wolfCrypt.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PICO_OK 0
#define PICO_ERROR_RESOURCE_IN_USE (-13)
#define SHA256_RESULT_BYTES 32

enum sha256_endianness { SHA256_LITTLE_ENDIAN, SHA256_BIG_ENDIAN };

typedef union {
    uint32_t words[SHA256_RESULT_BYTES / 4];
    uint8_t bytes[SHA256_RESULT_BYTES];
} sha256_result_t;

typedef struct pico_sha256_state {
    enum sha256_endianness endianness;
    bool locked;
} pico_sha256_state_t;

int pico_sha256_try_start(pico_sha256_state_t *state, enum sha256_endianness endianness,
                          bool use_dma);
void pico_sha256_update_blocking(pico_sha256_state_t *state, const uint8_t *data,
                                 size_t data_size_bytes);
void pico_sha256_finish(pico_sha256_state_t *state, sha256_result_t *out);
