// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/seal.c (AES-256-GCM sealing, with wolfCrypt built for the host and
// cross-checked against OpenSSL's libcrypto), for src/core/config.c sealing through it, and for
// src/ssh/hostkey_record.c (the SSH host key's record).

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/evp.h>

#include "core/config.h"
#include "core/seal.h"
#include "ssh/hostkey_record.h"
#include "test.h"

// --- a seal context with a known key and predictable "random" nonces ------------------------------

static uint8_t next_random;
static bool random_fails;
static int random_calls;

static bool f_random(void *ctx, uint8_t *out, size_t len) {
    (void)ctx;
    random_calls++;
    if (random_fails)
        return false;
    for (size_t i = 0; i < len; i++)
        out[i] = next_random++;
    return true;
}

static seal_ctx_t ctx;
static sealer_t sealer;

static void start(void) {
    memset(&ctx, 0, sizeof(ctx));
    for (unsigned i = 0; i < SEAL_KEY_LEN; i++)
        ctx.key[i] = (uint8_t)(0xa0 + i);
    ctx.have_key = true;
    ctx.random = f_random;
    next_random = 0x10;
    random_fails = false;
    random_calls = 0;
    sealer = seal_sealer(&ctx);
}

static void unhex(const char *hex, uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

// AES-256-GCM with OpenSSL: an implementation that shares nothing with the code under test.
static void openssl_gcm(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad,
                        size_t aad_len, const uint8_t *plain, size_t len, uint8_t *cipher,
                        uint8_t tag[16]) {
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int n = 0, m = 0;
    CHECK(c != NULL);
    CHECK(EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1);
    CHECK(EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) == 1);
    CHECK(EVP_EncryptInit_ex(c, NULL, NULL, key, nonce) == 1);
    CHECK(EVP_EncryptUpdate(c, NULL, &n, aad, (int)aad_len) == 1);
    CHECK(EVP_EncryptUpdate(c, cipher, &n, plain, (int)len) == 1);
    CHECK(EVP_EncryptFinal_ex(c, cipher + n, &m) == 1);
    CHECK_EQ((size_t)(n + m), len);
    CHECK(EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1);
    EVP_CIPHER_CTX_free(c);
}

// --- seal.c -------------------------------------------------------------------------------------------

// The blob's layout: the nonce the random source gave, the ciphertext, the tag; AES-256-GCM
// with the associated data authenticated and not stored. Checked against OpenSSL.
TEST(blob_layout_matches_openssl) {
    start();
    static const uint8_t aad[12] = { 'S', 'O', 'S', 'C', 2, 0, 0x6c, 0x06, 9, 0, 0, 0 };
    uint8_t plain[100], blob[100 + SEALER_OVERHEAD], want[100], want_tag[16], nonce[12];
    for (unsigned i = 0; i < sizeof(plain); i++)
        plain[i] = (uint8_t)(i * 7);
    for (unsigned i = 0; i < 12; i++)
        nonce[i] = (uint8_t)(0x10 + i);
    CHECK(seal_seal(&ctx, aad, sizeof(aad), plain, sizeof(plain), blob));
    CHECK_EQ(random_calls, 1);
    openssl_gcm(ctx.key, nonce, aad, sizeof(aad), plain, sizeof(plain), want, want_tag);
    CHECK_MEM(blob, nonce, 12);
    CHECK_MEM(blob + 12, want, sizeof(want));
    CHECK_MEM(blob + 12 + sizeof(plain), want_tag, 16);
}

// The same, as fixed bytes: key a0..bf, nonce 10..1b, associated data "header", data "sealed
// by SOS". A change to the cipher, the nonce's place or what is authenticated changes these.
// Computed with OpenSSL alone (EVP_aes_256_gcm), outside this test.
#define KNOWN_ANSWER \
    "101112131415161718191a1be7d31462bb2395d0e3593f5ecaf224a5bcf464831b151bf0dafbcae869"

TEST(known_answer) {
    start();
    static const uint8_t aad[6] = { 'h', 'e', 'a', 'd', 'e', 'r' };
    static const uint8_t plain[13] = { 's', 'e', 'a', 'l', 'e', 'd', ' ', 'b', 'y', ' ', 'S', 'O', 'S' };
    uint8_t blob[13 + SEALER_OVERHEAD], want[13 + SEALER_OVERHEAD];
    unhex(KNOWN_ANSWER, want, sizeof(want));
    CHECK(seal_seal(&ctx, aad, sizeof(aad), plain, sizeof(plain), blob));
    CHECK_MEM(blob, want, sizeof(want));
}

TEST(round_trip) {
    start();
    uint8_t plain[300], blob[300 + SEALER_OVERHEAD], back[300];
    for (unsigned i = 0; i < sizeof(plain); i++)
        plain[i] = (uint8_t)(i ^ 0x33);
    CHECK(seal_seal(&ctx, (const uint8_t *)"aad", 3, plain, sizeof(plain), blob));
    memset(back, 0xaa, sizeof(back));
    CHECK(seal_open(&ctx, (const uint8_t *)"aad", 3, blob, sizeof(blob), back));
    CHECK_MEM(back, plain, sizeof(plain));
    CHECK(seal_seal(&ctx, NULL, 0, plain, 1, blob));            // no associated data, one byte
    CHECK(seal_open(&ctx, NULL, 0, blob, 1 + SEALER_OVERHEAD, back));
    CHECK_EQ(back[0], plain[0]);
}

TEST(every_seal_takes_a_fresh_nonce) {
    start();
    uint8_t plain[16] = { 0 }, a[16 + SEALER_OVERHEAD], b[16 + SEALER_OVERHEAD];
    CHECK(seal_seal(&ctx, NULL, 0, plain, sizeof(plain), a));
    CHECK(seal_seal(&ctx, NULL, 0, plain, sizeof(plain), b));
    CHECK_EQ(random_calls, 2);
    CHECK(memcmp(a, b, 12) != 0);                               // the nonces
    CHECK(memcmp(a + 12, b + 12, 16) != 0);                     // so the ciphertexts too
}

static bool all_zero(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i])
            return false;
    return true;
}

// Every single bit of the blob matters, and so does every bit of the associated data.
TEST(any_change_fails_to_open) {
    start();
    uint8_t aad[12] = { 'h', 'e', 'a', 'd', 'e', 'r', ' ', 'b', 'y', 't', 'e', 's' }, plain[40], blob[40 + SEALER_OVERHEAD], back[40];
    memset(plain, 0x42, sizeof(plain));
    CHECK(seal_seal(&ctx, aad, sizeof(aad), plain, sizeof(plain), blob));
    for (size_t i = 0; i < sizeof(blob); i++)
        for (unsigned bit = 0; bit < 8; bit++) {
            blob[i] ^= (uint8_t)(1u << bit);
            memset(back, 0xaa, sizeof(back));
            CHECK(!seal_open(&ctx, aad, sizeof(aad), blob, sizeof(blob), back));
            CHECK(all_zero(back, sizeof(back)));                // nothing unauthenticated is left
            blob[i] ^= (uint8_t)(1u << bit);
        }
    for (size_t i = 0; i < sizeof(aad); i++) {
        aad[i] ^= 1;
        CHECK(!seal_open(&ctx, aad, sizeof(aad), blob, sizeof(blob), back));
        aad[i] ^= 1;
    }
    CHECK(!seal_open(&ctx, aad, sizeof(aad) - 1, blob, sizeof(blob), back));    // shorter
    CHECK(!seal_open(&ctx, NULL, 0, blob, sizeof(blob), back));                 // none
    CHECK(!seal_open(&ctx, aad, sizeof(aad), blob, sizeof(blob) - 1, back));    // cut short
    CHECK(seal_open(&ctx, aad, sizeof(aad), blob, sizeof(blob), back));         // unchanged
    CHECK_MEM(back, plain, sizeof(plain));
}

TEST(another_key_fails_to_open) {
    start();
    uint8_t plain[40], blob[40 + SEALER_OVERHEAD], back[40];
    memset(plain, 0x42, sizeof(plain));
    CHECK(seal_seal(&ctx, NULL, 0, plain, sizeof(plain), blob));
    ctx.key[31] ^= 1;
    CHECK(!seal_open(&ctx, NULL, 0, blob, sizeof(blob), back));
    CHECK(all_zero(back, sizeof(back)));
}

TEST(no_key_or_no_random_numbers) {
    start();
    uint8_t plain[40], blob[40 + SEALER_OVERHEAD], good[40 + SEALER_OVERHEAD], back[40];
    memset(plain, 0x42, sizeof(plain));
    CHECK(seal_seal(&ctx, NULL, 0, plain, sizeof(plain), good));
    random_fails = true;
    memset(blob, 0xaa, sizeof(blob));
    CHECK(!seal_seal(&ctx, NULL, 0, plain, sizeof(plain), blob));
    CHECK(all_zero(blob, sizeof(blob)));
    random_fails = false;
    seal_wipe(&ctx);
    CHECK(!ctx.have_key);
    CHECK(all_zero(ctx.key, sizeof(ctx.key)));
    CHECK(!seal_seal(&ctx, NULL, 0, plain, sizeof(plain), blob));
    CHECK(!seal_open(&ctx, NULL, 0, good, sizeof(good), back));
    CHECK(!seal_open(&ctx, NULL, 0, good, SEALER_OVERHEAD, back));      // nothing sealed in it
}

// --- config.c through the real sealer -------------------------------------------------------------------

static uint8_t slots[2][CONFIG_SLOT_SIZE];

static const uint8_t *s_read(void *c, unsigned slot) { (void)c; return slots[slot]; }
static bool s_write(void *c, unsigned slot, const void *data, size_t len) {
    (void)c;
    memset(slots[slot], 0xff, CONFIG_SLOT_SIZE);
    memcpy(slots[slot], data, len);
    return true;
}

static bool flash_contains(const char *text) {
    size_t n = strlen(text);
    for (size_t i = 0; i + n <= sizeof(slots); i++)
        if (!memcmp((const uint8_t *)slots + i, text, n))
            return true;
    return false;
}

TEST(configuration_sealed_with_aes_gcm) {
    start();
    memset(slots, 0xff, sizeof(slots));
    const config_store_t plain_store = { s_read, s_write, NULL, NULL, false };
    const config_store_t store = { s_read, s_write, NULL, &sealer, false };
    sos_config_t a, b;
    uint32_t seq = 0;
    config_defaults(&a);
    strcpy(a.wifi_ssid, "HomeNet");
    strcpy(a.wifi_password, "correct horse");

    // As older firmware (or provisioning) left it: plain, readable in flash.
    CHECK(config_save(&plain_store, &a));
    CHECK(flash_contains("correct horse"));
    // The board loads it and saves it sealed; the plain copy is gone.
    CHECK(config_load(&store, &b, NULL));
    CHECK(config_save(&store, &b));
    CHECK(!flash_contains("correct horse"));
    CHECK(!flash_contains("HomeNet"));
    CHECK_EQ(config_stored(&store), CONFIG_STORED_SEALED);
    memset(&b, 0, sizeof(b));
    CHECK(config_load(&store, &b, &seq));
    CHECK_EQ(seq, 2);
    CHECK_MEM(&a, &b, sizeof(a));

    // The record's header is what's authenticated: the payload opens by hand with the first 12
    // bytes of the slot as associated data, and gives the configuration.
    static sos_config_t by_hand;
    CHECK(seal_open(&ctx, slots[1], 12, slots[1] + 16, sizeof(sos_config_t) + SEALER_OVERHEAD,
                    (uint8_t *)&by_hand));
    CHECK_MEM(&by_hand, &a, sizeof(a));

    // Another root secret (another key): no configuration, and the record is left alone.
    ctx.key[0] ^= 1;
    CHECK(!config_load(&store, &b, NULL));
    CHECK(config_sealed_unopened(&store));
    ctx.key[0] ^= 1;
    CHECK(config_load(&store, &b, NULL));
}

// --- the host key's record -----------------------------------------------------------------------------

static uint32_t fnv1a(const uint8_t *p, uint32_t n) {
    uint32_t h = 2166136261u;
    while (n--)
        h = (h ^ *p++) * 16777619u;
    return h;
}

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// A record as firmware from before sealing wrote it: magic, version 1, length, FNV-1a, the key,
// the rest of the page erased.
static void old_record(uint8_t page[HOSTREC_PAGE], const uint8_t *der, uint32_t len) {
    memset(page, 0xff, HOSTREC_PAGE);
    memcpy(page, "SOSK", 4);
    put_le32(page + 4, 1);
    put_le32(page + 8, len);
    put_le32(page + 12, fnv1a(der, len));
    memcpy(page + 16, der, len);
}

static void sample_key(uint8_t *der, uint32_t len) {
    for (uint32_t i = 0; i < len; i++)
        der[i] = (uint8_t)(0x30 + i * 3);
}

TEST(hostkey_old_plain_record_is_read) {
    start();
    uint8_t page[HOSTREC_PAGE], der[240], out[HOSTREC_PAGE];
    uint32_t len = 0;
    sample_key(der, sizeof(der));
    old_record(page, der, 82);
    CHECK_EQ(hostrec_kind(page), HOSTREC_PLAIN);
    CHECK(hostrec_open(page, NULL, false, out, &len));             // no sealer needed
    CHECK_EQ(len, 82);
    CHECK_MEM(out, der, 82);
    // A secured board doesn't use a plain record, with or without a sealer.
    CHECK(!hostrec_open(page, NULL, true, out, &len));
    CHECK(!hostrec_open(page, &sealer, true, out, &len));
    CHECK_EQ(len, 0);
    CHECK(all_zero(out, sizeof(out)));
    old_record(page, der, 240);                             // the longest a plain record held
    CHECK_EQ(hostrec_kind(page), HOSTREC_PLAIN);
    page[20] ^= 1;                                          // damaged: the check fails
    CHECK_EQ(hostrec_kind(page), HOSTREC_NONE);
    CHECK(!hostrec_open(page, &sealer, false, out, &len));
    CHECK_EQ(len, 0);
}

TEST(hostkey_sealed_round_trip_and_layout) {
    start();
    uint8_t page[HOSTREC_PAGE], der[82], out[HOSTREC_PAGE];
    uint32_t len = 0;
    sample_key(der, sizeof(der));
    CHECK(hostrec_seal(page, &sealer, der, sizeof(der)));
    static const uint8_t head[12] = { 'S', 'O', 'S', 'K', 2, 0, 0, 0, 82, 0, 0, 0 };
    CHECK_MEM(page, head, sizeof(head));
    for (unsigned i = 12 + 82 + SEALER_OVERHEAD; i < HOSTREC_PAGE; i++)
        CHECK_EQ(page[i], 0xff);
    // The key isn't readable in the page.
    for (unsigned i = 0; i + 8 <= HOSTREC_PAGE; i++)
        CHECK(memcmp(page + i, der + 10, 8) != 0);
    CHECK_EQ(hostrec_kind(page), HOSTREC_SEALED);
    CHECK(hostrec_open(page, &sealer, false, out, &len));
    CHECK_EQ(len, sizeof(der));
    CHECK_MEM(out, der, sizeof(der));
    CHECK(hostrec_open(page, &sealer, true, out, &len));    // a secured board reads it too
    CHECK_MEM(out, der, sizeof(der));
    // The header is the associated data: the blob opens by hand with the first 12 bytes.
    uint8_t by_hand[82];
    CHECK(seal_open(&ctx, page, 12, page + 12, 82 + SEALER_OVERHEAD, by_hand));
    CHECK_MEM(by_hand, der, sizeof(der));
}

TEST(hostkey_sealed_record_that_does_not_open) {
    start();
    uint8_t page[HOSTREC_PAGE], der[82], out[HOSTREC_PAGE];
    uint32_t len = 7;
    sample_key(der, sizeof(der));
    CHECK(hostrec_seal(page, &sealer, der, sizeof(der)));

    CHECK(!hostrec_open(page, NULL, false, out, &len));            // no sealer
    CHECK_EQ(len, 0);
    CHECK(all_zero(out, sizeof(out)));

    page[60] ^= 1;                                          // changed
    CHECK_EQ(hostrec_kind(page), HOSTREC_SEALED);           // still looks like one
    CHECK(!hostrec_open(page, &sealer, false, out, &len));
    CHECK(all_zero(out, sizeof(out)));
    page[60] ^= 1;

    put_le32(page + 8, 81);                                 // another length in the header
    CHECK(!hostrec_open(page, &sealer, false, out, &len));
    put_le32(page + 8, 82);

    ctx.key[5] ^= 1;                                        // another root secret
    CHECK(!hostrec_open(page, &sealer, false, out, &len));
    ctx.key[5] ^= 1;
    CHECK(hostrec_open(page, &sealer, false, out, &len));
}

TEST(hostkey_record_limits) {
    start();
    uint8_t page[HOSTREC_PAGE], der[HOSTREC_PAGE], out[HOSTREC_PAGE];
    uint32_t len = 0;
    sample_key(der, sizeof(der));
    CHECK(hostrec_seal(page, &sealer, der, HOSTREC_MAX_KEY));       // fills the page exactly
    CHECK(hostrec_open(page, &sealer, false, out, &len));
    CHECK_EQ(len, HOSTREC_MAX_KEY);
    CHECK_MEM(out, der, HOSTREC_MAX_KEY);
    CHECK(!hostrec_seal(page, &sealer, der, HOSTREC_MAX_KEY + 1));
    CHECK(all_zero(page, sizeof(page)));
    CHECK(!hostrec_seal(page, &sealer, der, 0));
    CHECK(!hostrec_seal(page, NULL, der, 82));
    random_fails = true;
    CHECK(!hostrec_seal(page, &sealer, der, 82));
    CHECK(all_zero(page, sizeof(page)));
}

TEST(hostkey_pages_that_are_not_records) {
    uint8_t page[HOSTREC_PAGE], out[HOSTREC_PAGE];
    uint32_t len = 0;
    start();
    memset(page, 0xff, sizeof(page));                       // erased
    CHECK_EQ(hostrec_kind(page), HOSTREC_NONE);
    CHECK(!hostrec_open(page, &sealer, false, out, &len));
    memset(page, 0, sizeof(page));
    CHECK_EQ(hostrec_kind(page), HOSTREC_NONE);
    memcpy(page, "SOSK", 4);
    put_le32(page + 4, 3);                                  // a version from the future
    put_le32(page + 8, 82);
    CHECK_EQ(hostrec_kind(page), HOSTREC_NONE);
    put_le32(page + 4, 2);
    put_le32(page + 8, HOSTREC_MAX_KEY + 1);                // a length that can't fit
    CHECK_EQ(hostrec_kind(page), HOSTREC_NONE);
    CHECK(!hostrec_open(page, &sealer, false, out, &len));
    put_le32(page + 8, 0);
    CHECK_EQ(hostrec_kind(page), HOSTREC_NONE);
    put_le32(page + 4, 1);
    put_le32(page + 8, 241);                                // too long for a plain record
    CHECK_EQ(hostrec_kind(page), HOSTREC_NONE);
}

TEST_MAIN(
    RUN(blob_layout_matches_openssl);
    RUN(known_answer);
    RUN(round_trip);
    RUN(every_seal_takes_a_fresh_nonce);
    RUN(any_change_fails_to_open);
    RUN(another_key_fails_to_open);
    RUN(no_key_or_no_random_numbers);
    RUN(configuration_sealed_with_aes_gcm);
    RUN(hostkey_old_plain_record_is_read);
    RUN(hostkey_sealed_round_trip_and_layout);
    RUN(hostkey_sealed_record_that_does_not_open);
    RUN(hostkey_record_limits);
    RUN(hostkey_pages_that_are_not_records);
)
