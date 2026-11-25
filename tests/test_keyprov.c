// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/keyprov.c (key derivation from the root secret, and the lock) and
// src/core/rootsecret.c (the root secret's flash record), with wolfCrypt built for the host.

#include <string.h>

#include "core/keyprov.h"
#include "core/rootsecret.h"
#include "test.h"

// --- HKDF: RFC 5869 test vectors (SHA-256) ---------------------------------------------------------

static void unhex(const char *hex, uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

TEST(hkdf_rfc5869_case_1) {
    uint8_t ikm[22], salt[13], info[10], okm[42], want[42];
    memset(ikm, 0x0b, sizeof(ikm));
    for (unsigned i = 0; i < sizeof(salt); i++)
        salt[i] = (uint8_t)i;
    for (unsigned i = 0; i < sizeof(info); i++)
        info[i] = (uint8_t)(0xf0 + i);
    unhex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865",
          want, sizeof(want));
    CHECK(keyprov_hkdf(ikm, sizeof(ikm), salt, sizeof(salt), info, sizeof(info), okm, sizeof(okm)));
    CHECK_MEM(okm, want, sizeof(want));
}

TEST(hkdf_rfc5869_case_2) {
    uint8_t ikm[80], salt[80], info[80], okm[82], want[82];
    for (unsigned i = 0; i < 80; i++) {
        ikm[i] = (uint8_t)i;
        salt[i] = (uint8_t)(0x60 + i);
        info[i] = (uint8_t)(0xb0 + i);
    }
    unhex("b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c59045a99cac7827271cb41c6"
          "5e590e09da3275600c2f09b8367793a9aca3db71cc30c58179ec3e87c14c01d5c1f3434f1d87",
          want, sizeof(want));
    CHECK(keyprov_hkdf(ikm, sizeof(ikm), salt, sizeof(salt), info, sizeof(info), okm, sizeof(okm)));
    CHECK_MEM(okm, want, sizeof(want));
}

// No salt and no info: what keyprov_derive relies on for the salt.
TEST(hkdf_rfc5869_case_3) {
    uint8_t ikm[22], okm[42], want[42];
    memset(ikm, 0x0b, sizeof(ikm));
    unhex("8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8",
          want, sizeof(want));
    CHECK(keyprov_hkdf(ikm, sizeof(ikm), NULL, 0, NULL, 0, okm, sizeof(okm)));
    CHECK_MEM(okm, want, sizeof(want));
}

// --- the provider ------------------------------------------------------------------------------------

static uint8_t fake_root[KEYPROV_ROOT_LEN];
static bool have_root, hw_locked;
static int root_reads, lock_calls;

static bool f_root(void *ctx, uint8_t out[KEYPROV_ROOT_LEN]) {
    (void)ctx;
    root_reads++;
    if (!have_root || hw_locked)
        return false;
    memcpy(out, fake_root, KEYPROV_ROOT_LEN);
    return true;
}
static void f_lock(void *ctx) { (void)ctx; lock_calls++; hw_locked = true; }

static const keyprov_backend_t fake_backend = { f_root, f_lock, NULL };

static void start(void) {
    for (unsigned i = 0; i < KEYPROV_ROOT_LEN; i++)
        fake_root[i] = (uint8_t)i;
    have_root = true;
    hw_locked = false;
    root_reads = lock_calls = 0;
    keyprov_init(&fake_backend);
}

static bool all_zero(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i])
            return false;
    return true;
}

// The keys for root secret 00 01 .. 1f, computed independently (Python hmac/hashlib):
// HKDF-SHA256, no salt, info = the label, 32 bytes. These pin the labels: a changed label, salt
// or hash changes the keys, and everything already sealed on a board could no longer be opened.
static const char *const known[KEYPROV_PURPOSES] = {
    [KEYPROV_CONFIG_SEAL]      = "41dbd79babd1d214f590c73f780cf881ec7b3213018e776ca981a5933d7d5eef",
    [KEYPROV_HOSTKEY_SEAL]     = "fcedf13ff40fe0df04a72b35882f77ee5189d50737419ac689145ea05671961c",
    [KEYPROV_CONSOLE_PASSWORD] = "affba2708c2332c7529d9ecb6436349f289f0e30de1a057e389722e8220e4072",
    [KEYPROV_SD_DEVICE]        = "119125edf4b82450ddee3f4503d629c0f566f858b34fb560786b2f58f8724221",
    [KEYPROV_SD_WRAP_BASE]     = "20f5fc197b743e02cb731f9aa3c803959d39b29f08e21f92b465b52f9c254e0e",
};

TEST(derive_known_answers) {
    start();
    for (unsigned p = 0; p < KEYPROV_PURPOSES; p++) {
        uint8_t key[KEYPROV_KEY_LEN], want[KEYPROV_KEY_LEN];
        CHECK(known[p] != NULL);
        unhex(known[p], want, sizeof(want));
        CHECK(keyprov_derive(p, key));
        CHECK_MEM(key, want, sizeof(want));
    }
}

TEST(labels_are_distinct_and_none_prefixes_another) {
    for (unsigned a = 0; a < KEYPROV_PURPOSES; a++) {
        CHECK(keyprov_label(a) != NULL);
        for (unsigned b = 0; b < KEYPROV_PURPOSES; b++)
            if (a != b)
                CHECK(strncmp(keyprov_label(a), keyprov_label(b), strlen(keyprov_label(a))) != 0);
    }
    CHECK(keyprov_label(KEYPROV_PURPOSES) == NULL);
}

TEST(purposes_give_different_keys) {
    start();
    uint8_t keys[KEYPROV_PURPOSES][KEYPROV_KEY_LEN];
    for (unsigned p = 0; p < KEYPROV_PURPOSES; p++)
        CHECK(keyprov_derive(p, keys[p]));
    for (unsigned a = 0; a < KEYPROV_PURPOSES; a++)
        for (unsigned b = a + 1; b < KEYPROV_PURPOSES; b++)
            CHECK(memcmp(keys[a], keys[b], KEYPROV_KEY_LEN) != 0);
}

TEST(same_purpose_gives_the_same_key) {
    start();
    uint8_t a[KEYPROV_KEY_LEN], b[KEYPROV_KEY_LEN];
    CHECK(keyprov_derive(KEYPROV_CONFIG_SEAL, a));
    CHECK(keyprov_derive(KEYPROV_CONFIG_SEAL, b));
    CHECK_MEM(a, b, sizeof(a));
}

TEST(another_root_gives_other_keys) {
    start();
    uint8_t a[KEYPROV_KEY_LEN], b[KEYPROV_KEY_LEN];
    CHECK(keyprov_derive(KEYPROV_CONFIG_SEAL, a));
    fake_root[31] ^= 1;
    CHECK(keyprov_derive(KEYPROV_CONFIG_SEAL, b));
    CHECK(memcmp(a, b, sizeof(a)) != 0);
}

TEST(unknown_purpose_fails) {
    start();
    uint8_t key[KEYPROV_KEY_LEN];
    memset(key, 0xaa, sizeof(key));
    CHECK(!keyprov_derive(KEYPROV_PURPOSES, key));
    CHECK(all_zero(key, sizeof(key)));
    CHECK(!keyprov_derive(~0u, key));
    CHECK_EQ(root_reads, 0);                // the root secret isn't even read
}

TEST(no_root_fails_and_zeroes_the_key) {
    start();
    have_root = false;
    uint8_t key[KEYPROV_KEY_LEN];
    memset(key, 0xaa, sizeof(key));
    CHECK(!keyprov_derive(KEYPROV_CONFIG_SEAL, key));
    CHECK(all_zero(key, sizeof(key)));
}

TEST(derive_fails_after_lock) {
    start();
    uint8_t key[KEYPROV_KEY_LEN];
    CHECK(!keyprov_locked());
    CHECK(keyprov_derive(KEYPROV_SD_DEVICE, key));
    keyprov_lock();
    CHECK(keyprov_locked());
    CHECK_EQ(lock_calls, 1);
    int reads = root_reads;
    memset(key, 0xaa, sizeof(key));
    for (unsigned p = 0; p < KEYPROV_PURPOSES; p++) {
        CHECK(!keyprov_derive(p, key));
        CHECK(all_zero(key, sizeof(key)));
    }
    CHECK_EQ(root_reads, reads);            // the backend isn't asked again
    keyprov_lock();                         // locking twice locks the backend once
    CHECK_EQ(lock_calls, 1);
}

TEST(derive_fails_before_init) {
    keyprov_init(NULL);
    uint8_t key[KEYPROV_KEY_LEN];
    CHECK(!keyprov_derive(KEYPROV_CONFIG_SEAL, key));
    keyprov_lock();                         // no backend: only the flag
    CHECK(keyprov_locked());
}

static const keyprov_backend_t no_lock_backend = { f_root, NULL, NULL };

TEST(backend_without_a_lock) {
    start();
    keyprov_init(&no_lock_backend);
    uint8_t key[KEYPROV_KEY_LEN];
    CHECK(keyprov_derive(KEYPROV_CONFIG_SEAL, key));
    keyprov_lock();
    CHECK(!keyprov_derive(KEYPROV_CONFIG_SEAL, key));
}

// --- the root secret's record ------------------------------------------------------------------------

static uint8_t flash[ROOTSECRET_PAGE];
static int writes;
static bool write_fails, write_tears, random_fails;
static uint8_t next_random;

static const uint8_t *s_read(void *ctx) { (void)ctx; return flash; }
static bool s_write(void *ctx, const uint8_t *page) {
    (void)ctx;
    writes++;
    if (write_fails)
        return false;
    if (!page) {
        memset(flash, 0xff, sizeof(flash));
        return true;
    }
    memcpy(flash, page, sizeof(flash));
    if (write_tears)                        // power lost partway: the rest stays erased
        memset(flash + 20, 0xff, sizeof(flash) - 20);
    return true;
}
static bool s_random(void *ctx, uint8_t *out, size_t len) {
    (void)ctx;
    if (random_fails)
        return false;
    for (size_t i = 0; i < len; i++)
        out[i] = next_random++;
    return true;
}

static const rootsecret_store_t fake_store = { s_read, s_write, s_random, NULL };

static void blank(void) {
    memset(flash, 0xff, sizeof(flash));
    writes = 0;
    write_fails = write_tears = random_fails = false;
    next_random = 0x40;
}

TEST(blank_page_gets_a_secret) {
    blank();
    CHECK_EQ(rootsecret_state(&fake_store), ROOTSECRET_NONE);
    bool created = false;
    CHECK_EQ(rootsecret_ensure(&fake_store, &created), ROOTSECRET_OK);
    CHECK(created);
    CHECK_EQ(writes, 1);
    uint8_t secret[ROOTSECRET_LEN];
    CHECK(rootsecret_read(&fake_store, secret));
    for (unsigned i = 0; i < ROOTSECRET_LEN; i++)
        CHECK_EQ(secret[i], 0x40 + i);
}

// The stored layout, byte for byte: magic "SOSR", version 1, the secret, FNV-1a of the secret,
// and the rest of the page erased. Released firmware has to keep reading this.
TEST(record_layout) {
    blank();
    CHECK_EQ(rootsecret_ensure(&fake_store, NULL), ROOTSECRET_OK);
    static const uint8_t head[8] = { 'S', 'O', 'S', 'R', 1, 0, 0, 0 };
    CHECK_MEM(flash, head, sizeof(head));
    for (unsigned i = 0; i < ROOTSECRET_LEN; i++)
        CHECK_EQ(flash[8 + i], 0x40 + i);
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < ROOTSECRET_LEN; i++)
        h = (h ^ flash[8 + i]) * 16777619u;
    CHECK_EQ(flash[40], h & 0xff);
    CHECK_EQ(flash[41], (h >> 8) & 0xff);
    CHECK_EQ(flash[42], (h >> 16) & 0xff);
    CHECK_EQ(flash[43], h >> 24);
    for (unsigned i = 44; i < ROOTSECRET_PAGE; i++)
        CHECK_EQ(flash[i], 0xff);
}

TEST(an_existing_secret_is_kept) {
    blank();
    CHECK_EQ(rootsecret_ensure(&fake_store, NULL), ROOTSECRET_OK);
    uint8_t before[ROOTSECRET_PAGE];
    memcpy(before, flash, sizeof(before));
    bool created = true;
    CHECK_EQ(rootsecret_ensure(&fake_store, &created), ROOTSECRET_OK);
    CHECK(!created);
    CHECK_EQ(writes, 1);
    CHECK_MEM(flash, before, sizeof(before));
}

// Anything that isn't blank and isn't a valid record is left exactly as it is.
static void check_left_alone(void) {
    uint8_t before[ROOTSECRET_PAGE], secret[ROOTSECRET_LEN];
    memcpy(before, flash, sizeof(before));
    writes = 0;
    bool created = true;
    CHECK_EQ(rootsecret_state(&fake_store), ROOTSECRET_INVALID);
    CHECK_EQ(rootsecret_ensure(&fake_store, &created), ROOTSECRET_INVALID);
    CHECK(!created);
    CHECK_EQ(writes, 0);
    CHECK_MEM(flash, before, sizeof(before));
    memset(secret, 0xaa, sizeof(secret));
    CHECK(!rootsecret_read(&fake_store, secret));
    CHECK(all_zero(secret, sizeof(secret)));
}

TEST(damaged_record_is_left_alone) {
    blank();
    CHECK_EQ(rootsecret_ensure(&fake_store, NULL), ROOTSECRET_OK);
    flash[20] ^= 1;                         // a bit of the secret
    check_left_alone();
}

TEST(bad_check_is_left_alone) {
    blank();
    CHECK_EQ(rootsecret_ensure(&fake_store, NULL), ROOTSECRET_OK);
    flash[40] ^= 0x80;
    check_left_alone();
}

TEST(newer_version_is_left_alone) {
    blank();
    CHECK_EQ(rootsecret_ensure(&fake_store, NULL), ROOTSECRET_OK);
    flash[4] = 2;
    check_left_alone();
}

TEST(unknown_contents_are_left_alone) {
    blank();
    flash[0] = 0;                           // one byte that isn't erased, no magic
    check_left_alone();
    blank();
    flash[ROOTSECRET_PAGE - 1] = 0x7f;      // even at the end of the page
    check_left_alone();
    memset(flash, 0, sizeof(flash));
    check_left_alone();
}

TEST(no_random_numbers_no_secret) {
    blank();
    random_fails = true;
    bool created = true;
    CHECK_EQ(rootsecret_ensure(&fake_store, &created), ROOTSECRET_NONE);
    CHECK(!created);
    CHECK_EQ(writes, 0);
}

TEST(failed_write_leaves_none) {
    blank();
    write_fails = true;
    bool created = true;
    CHECK_EQ(rootsecret_ensure(&fake_store, &created), ROOTSECRET_NONE);
    CHECK(!created);
    write_fails = false;                    // the next boot tries again
    CHECK_EQ(rootsecret_ensure(&fake_store, &created), ROOTSECRET_OK);
    CHECK(created);
}

// A write cut short leaves part of a record: not usable, and not replaced on its own.
TEST(torn_write_is_invalid_and_stays) {
    blank();
    write_tears = true;
    bool created = true;
    CHECK_EQ(rootsecret_ensure(&fake_store, &created), ROOTSECRET_INVALID);
    CHECK(!created);
    write_tears = false;
    check_left_alone();
}

TEST(forget_then_a_new_secret) {
    blank();
    CHECK_EQ(rootsecret_ensure(&fake_store, NULL), ROOTSECRET_OK);
    uint8_t first[ROOTSECRET_LEN], second[ROOTSECRET_LEN];
    CHECK(rootsecret_read(&fake_store, first));
    CHECK(rootsecret_forget(&fake_store));
    CHECK_EQ(rootsecret_state(&fake_store), ROOTSECRET_NONE);
    CHECK(!rootsecret_read(&fake_store, second));
    CHECK_EQ(rootsecret_ensure(&fake_store, NULL), ROOTSECRET_OK);
    CHECK(rootsecret_read(&fake_store, second));
    CHECK(memcmp(first, second, sizeof(first)) != 0);
}

TEST(forget_clears_an_invalid_record) {
    blank();
    memset(flash, 0, sizeof(flash));
    CHECK(rootsecret_forget(&fake_store));
    CHECK_EQ(rootsecret_state(&fake_store), ROOTSECRET_NONE);
}

TEST(failed_forget_says_so) {
    blank();
    CHECK_EQ(rootsecret_ensure(&fake_store, NULL), ROOTSECRET_OK);
    write_fails = true;
    CHECK(!rootsecret_forget(&fake_store));
    CHECK_EQ(rootsecret_state(&fake_store), ROOTSECRET_OK);
}

TEST_MAIN(
    RUN(hkdf_rfc5869_case_1);
    RUN(hkdf_rfc5869_case_2);
    RUN(hkdf_rfc5869_case_3);
    RUN(derive_known_answers);
    RUN(labels_are_distinct_and_none_prefixes_another);
    RUN(purposes_give_different_keys);
    RUN(same_purpose_gives_the_same_key);
    RUN(another_root_gives_other_keys);
    RUN(unknown_purpose_fails);
    RUN(no_root_fails_and_zeroes_the_key);
    RUN(derive_fails_after_lock);
    RUN(derive_fails_before_init);
    RUN(backend_without_a_lock);
    RUN(blank_page_gets_a_secret);
    RUN(record_layout);
    RUN(an_existing_secret_is_kept);
    RUN(damaged_record_is_left_alone);
    RUN(bad_check_is_left_alone);
    RUN(newer_version_is_left_alone);
    RUN(unknown_contents_are_left_alone);
    RUN(no_random_numbers_no_secret);
    RUN(failed_write_leaves_none);
    RUN(torn_write_is_invalid_and_stays);
    RUN(forget_then_a_new_secret);
    RUN(forget_clears_an_invalid_record);
    RUN(failed_forget_says_so);
)
