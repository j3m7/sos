// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The SHA-256 block: see sha256hw.h. Fed by the CPU, not DMA: a job step hashes one 4 KB chunk
// at a time and waits for it anyway, and DMA would claim a channel the SD card's SPI driver and
// the Wi-Fi chip's PIO also claim from. The host tests build this file against a fake
// pico_sha256 made with wolfCrypt (tests/fake_sha256.c).

#include "files/sha256hw.h"

#include <string.h>

#include "pico/sha256.h"

#include "core/log.h"

static pico_sha256_state_t state;       // the hash on the block: there is one block
static bool held;                       // a caller of sha256hw_begin() has it: tested before
                                        // pico_sha256_try_start(), which clears the state first

static enum { HW_UNTESTED, HW_GOOD, HW_BAD } health;

// FIPS 180-2 appendix B.1 (one block) and B.2 (the padding takes a second block).
static const struct {
    const char *msg;
    uint8_t digest[SHA256HW_DIGEST];
} vectors[] = {
    {"abc",
     {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22,
      0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00,
      0x15, 0xad}},
    {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
     {0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8, 0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60,
      0x39, 0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67, 0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb,
      0x06, 0xc1}},
};

static bool start(void) {
    return pico_sha256_try_start(&state, SHA256_BIG_ENDIAN, false) == PICO_OK;
}

// The vectors on the block. True if they all match; false if the block is taken (still
// untested) or gave a wrong answer (then never used again).
static bool self_check(void) {
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        if (!start())
            return false;
        pico_sha256_update_blocking(&state, (const uint8_t *)vectors[i].msg,
                                    strlen(vectors[i].msg));
        sha256_result_t r;
        pico_sha256_finish(&state, &r);
        if (memcmp(r.bytes, vectors[i].digest, SHA256HW_DIGEST)) {
            health = HW_BAD;
            log_msg(SOS_LOG_KERN | SOS_LOG_ERR,
                    "sha256: the SHA-256 block gave a wrong answer; hashing in software");
            return false;
        }
    }
    health = HW_GOOD;
    return true;
}

bool sha256hw_begin(void) {
    if (held || health == HW_BAD)
        return false;
    if (health == HW_UNTESTED && !self_check())
        return false;
    if (!start())
        return false;
    held = true;
    return true;
}

void sha256hw_update(const uint8_t *data, size_t len) {
    if (held)
        pico_sha256_update_blocking(&state, data, len);
}

void sha256hw_finish(uint8_t out[SHA256HW_DIGEST]) {
    if (!held) {
        memset(out, 0, SHA256HW_DIGEST);
        return;
    }
    sha256_result_t r;
    pico_sha256_finish(&state, &r);
    memcpy(out, r.bytes, SHA256HW_DIGEST);
    held = false;
}

void sha256hw_abort(void) {
    if (!held)
        return;
    pico_sha256_finish(&state, NULL);   // NULL: abandoned, the lock released
    held = false;
}
