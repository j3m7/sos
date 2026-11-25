// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/files/sdcrypt.c (SD card encryption: header and sectors), with wolfCrypt
// built for the host, cross-checked against tools/host/sdcrypt.c (OpenSSL's libcrypto).

#define _GNU_SOURCE            // memmem
#include <stdint.h>
#include <string.h>

#include "files/sdcrypt.h"
#include "test.h"

// tools/host/sdcrypt.c, built with SDCRYPT_TOOL_NO_MAIN.
int sdcrypt_tool_xts(int encrypt, const uint8_t key[32], uint64_t lsector,
                     const uint8_t *in, uint8_t *out, unsigned count);
int sdcrypt_tool_header(const uint8_t sector[512], uint32_t *offset);

static uint8_t devkey[32], other_devkey[32], card_id[16], nonce[12], data_key[32];

static void fill(uint8_t *p, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)(seed + i * 37);
}

static void setup(void) {
    fill(devkey, sizeof(devkey), 1);
    fill(other_devkey, sizeof(other_devkey), 2);
    fill(card_id, sizeof(card_id), 3);
    fill(nonce, sizeof(nonce), 4);
    fill(data_key, sizeof(data_key), 5);
}

TEST(header_round_trip) {
    setup();
    uint8_t sector[512];
    CHECK(sdcrypt_header_build(sector, devkey, card_id, nonce, data_key));
    CHECK(sdcrypt_is_header(sector));
    CHECK(!memcmp(sector, "SOS-CRYPT-CARD\0\0", 16));
    // The data key isn't stored in the clear.
    CHECK(memmem(sector, sizeof(sector), data_key, sizeof(data_key)) == NULL);
    for (int i = 100; i < 512; i++)
        CHECK_EQ(sector[i], 0);
    sdcrypt_info_t info;
    CHECK_EQ(sdcrypt_header_open(sector, devkey, &info), SDCRYPT_OK);
    CHECK_MEM(info.data_key, data_key, 32);
    CHECK_MEM(info.card_id, card_id, 16);
    CHECK_EQ(info.offset, SDCRYPT_OFFSET);
    // The host tool reads the same header.
    uint32_t offset = 0;
    CHECK_EQ(sdcrypt_tool_header(sector, &offset), 0);
    CHECK_EQ(offset, SDCRYPT_OFFSET);
}

TEST(header_rejected) {
    setup();
    uint8_t good[512], s[512];
    sdcrypt_info_t info;
    sdcrypt_header_build(good, devkey, card_id, nonce, data_key);

    // Another device's key: refused, nothing leaked into info.
    CHECK_EQ(sdcrypt_header_open(good, other_devkey, &info), SDCRYPT_WRONG_KEY);
    uint8_t zero[sizeof(info)] = {0};
    CHECK_MEM(&info, zero, sizeof(info));

    // Not a header: a plain card's MBR, or zeros.
    memset(s, 0, sizeof(s));
    s[510] = 0x55; s[511] = 0xaa;
    CHECK(!sdcrypt_is_header(s));
    CHECK_EQ(sdcrypt_header_open(s, devkey, &info), SDCRYPT_PLAIN);

    // Unknown version, cipher, wrap or offset.
    static const int fields[] = { 16, 17, 18, 20 };
    for (unsigned f = 0; f < sizeof(fields) / sizeof(fields[0]); f++) {
        memcpy(s, good, sizeof(s));
        s[fields[f]] ^= 0x02;
        CHECK_EQ(sdcrypt_header_open(s, devkey, &info), SDCRYPT_UNSUPPORTED);
    }

    // Any change to the authenticated fields, nonce, wrapped key or tag fails the tag.
    for (int i = 21; i < 100; i++) {           // (21-23: the offset, unsupported)
        memcpy(s, good, sizeof(s));
        s[i] ^= 0x01;
        int r = sdcrypt_header_open(s, devkey, &info);
        CHECK(r == SDCRYPT_WRONG_KEY || r == SDCRYPT_UNSUPPORTED);
        if (i >= 24)
            CHECK_EQ(r, SDCRYPT_WRONG_KEY);
    }
}

TEST(sectors_round_trip) {
    setup();
    static sdcrypt_t c;
    static uint8_t plain[8 * 512], enc[8 * 512], buf[8 * 512];
    fill(plain, sizeof(plain), 9);
    CHECK(!sdcrypt_encrypt(&c, enc, plain, 0, 1));     // not started
    CHECK(sdcrypt_start(&c, data_key));
    static const uint32_t starts[] = { 0, 1, 8191, 123456789, 0xfffffff8u };
    for (unsigned k = 0; k < sizeof(starts) / sizeof(starts[0]); k++) {
        CHECK(sdcrypt_encrypt(&c, enc, plain, starts[k], 8));
        CHECK(memcmp(enc, plain, 512) != 0);
        // Each sector differs even with the same plaintext (the tweak is the sector number).
        CHECK(memcmp(enc, enc + 512, 512) != 0 || memcmp(plain, plain + 512, 512) != 0);
        memcpy(buf, enc, sizeof(buf));
        CHECK(sdcrypt_decrypt(&c, buf, starts[k], 8));       // in place
        CHECK_MEM(buf, plain, sizeof(plain));
        // Decrypting at the wrong sector number gives garbage.
        memcpy(buf, enc, 512);
        CHECK(sdcrypt_decrypt(&c, buf, starts[k] + 1, 1));
        CHECK(memcmp(buf, plain, 512) != 0);
    }
    // Same plaintext in every sector: still different ciphertext per sector.
    memset(plain, 0, sizeof(plain));
    CHECK(sdcrypt_encrypt(&c, enc, plain, 100, 8));
    for (int i = 1; i < 8; i++)
        CHECK(memcmp(enc, enc + i * 512, 512) != 0);
    sdcrypt_stop(&c);
    CHECK(!c.ready);
    CHECK(!sdcrypt_decrypt(&c, buf, 0, 1));
}

TEST(matches_openssl) {
    setup();
    static sdcrypt_t c;
    static uint8_t plain[4 * 512], dev[4 * 512], ossl[4 * 512];
    fill(plain, sizeof(plain), 11);
    CHECK(sdcrypt_start(&c, data_key));
    static const uint64_t starts[] = { 0, 7, 8192, 4000000000ull };
    for (unsigned k = 0; k < sizeof(starts) / sizeof(starts[0]); k++) {
        // Device encrypts, OpenSSL (aes-128-xts, plain64 tweak) decrypts, and the other way.
        CHECK(sdcrypt_encrypt(&c, dev, plain, (uint32_t)starts[k], 4));
        CHECK_EQ(sdcrypt_tool_xts(1, data_key, starts[k], plain, ossl, 4), 0);
        CHECK_MEM(dev, ossl, sizeof(dev));
        CHECK_EQ(sdcrypt_tool_xts(0, data_key, starts[k], dev, ossl, 4), 0);
        CHECK_MEM(ossl, plain, sizeof(plain));
    }
    sdcrypt_stop(&c);
}

// FatFs hands the driver whatever buffer the caller gave f_read/f_write, at any alignment.
TEST(unaligned_buffers) {
    setup();
    static sdcrypt_t c;
    static uint8_t raw_plain[2 * 512 + 8], raw_enc[2 * 512 + 8], ref[2 * 512];
    CHECK(sdcrypt_start(&c, data_key));
    for (int off = 1; off < 8; off += 2) {
        uint8_t *p = raw_plain + off, *e = raw_enc + off;
        fill(p, 2 * 512, (uint8_t)off);
        CHECK(sdcrypt_encrypt(&c, e, p, 42, 2));
        CHECK_EQ(sdcrypt_tool_xts(1, data_key, 42, p, ref, 2), 0);
        CHECK_MEM(e, ref, sizeof(ref));
        CHECK(sdcrypt_decrypt(&c, e, 42, 2));
        CHECK_MEM(e, p, 2 * 512);
    }
    sdcrypt_stop(&c);
}

TEST(wipe) {
    uint8_t k[32];
    memset(k, 0xaa, sizeof(k));
    sdcrypt_wipe(k, sizeof(k));
    uint8_t zero[32] = {0};
    CHECK_MEM(k, zero, 32);
}

TEST_MAIN(
    RUN(header_round_trip);
    RUN(header_rejected);
    RUN(sectors_round_trip);
    RUN(matches_openssl);
    RUN(unaligned_buffers);
    RUN(wipe);
)
