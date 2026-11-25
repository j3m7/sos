// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/keyprov.c (key derivation from the root secret, and the lock) and
// src/core/rootsecret.c (the root secret's flash record), with wolfCrypt built for the host.

#include <string.h>

#include "core/keyprov.h"
#include "core/rootsecret.h"
#include "core/rootsecret_priv.h"
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

static const keyprov_backend_t no_root_backend = { NULL, NULL, NULL };

TEST(backend_without_a_root) {
    keyprov_test_reset();
    keyprov_init(&no_root_backend);
    uint8_t key[KEYPROV_KEY_LEN];
    CHECK(!keyprov_derive(KEYPROV_CONFIG_SEAL, key));
}

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
    keyprov_test_reset();
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
    CHECK(keyprov_derive(KEYPROV_SD_WRAP_BASE, key));
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

// Once started, another init can't swap the backend, and once locked it can't unlock.
static bool other_root(void *ctx, uint8_t out[KEYPROV_ROOT_LEN]) {
    (void)ctx;
    memset(out, 0x77, KEYPROV_ROOT_LEN);
    return true;
}
static const keyprov_backend_t other_backend = { other_root, NULL, NULL };

TEST(init_again_changes_nothing) {
    start();
    uint8_t a[KEYPROV_KEY_LEN], b[KEYPROV_KEY_LEN];
    CHECK(keyprov_derive(KEYPROV_CONFIG_SEAL, a));
    keyprov_init(&other_backend);
    CHECK(keyprov_derive(KEYPROV_CONFIG_SEAL, b));
    CHECK_MEM(a, b, sizeof(a));
    keyprov_lock();
    keyprov_init(&other_backend);
    keyprov_init(&fake_backend);
    CHECK(keyprov_locked());
    CHECK(!keyprov_derive(KEYPROV_CONFIG_SEAL, b));
    CHECK(all_zero(b, sizeof(b)));
}

TEST(derive_fails_before_init) {
    keyprov_test_reset();
    uint8_t key[KEYPROV_KEY_LEN];
    CHECK(!keyprov_derive(KEYPROV_CONFIG_SEAL, key));
    keyprov_lock();                         // no backend: only the flag
    CHECK(keyprov_locked());
}

static const keyprov_backend_t no_lock_backend = { f_root, NULL, NULL };

TEST(backend_without_a_lock) {
    start();
    keyprov_test_reset();
    keyprov_init(&no_lock_backend);
    uint8_t key[KEYPROV_KEY_LEN];
    CHECK(keyprov_derive(KEYPROV_CONFIG_SEAL, key));
    keyprov_lock();
    CHECK(!keyprov_derive(KEYPROV_CONFIG_SEAL, key));
}

// --- the root secret's record ------------------------------------------------------------------------

// A fake flash sector: programming can only clear bits, as in real flash.
static uint8_t flash[ROOTSECRET_SECTOR];
static int programs, erases;
static bool erase_fails, random_fails;
static int program_fails_at;            // this program call (1-based) does nothing; 0 = none
static int program_tears_at;            // this program call writes only the first 20 bytes
static uint8_t next_random;

#define PAGE(n) (flash + (n) * ROOTSECRET_PAGE)

static const uint8_t *s_read(void *ctx) { (void)ctx; return flash; }
static bool s_program(void *ctx, unsigned page, const uint8_t *data) {
    (void)ctx;
    programs++;
    CHECK(page < ROOTSECRET_SECTOR / ROOTSECRET_PAGE);
    for (unsigned i = 0; i < ROOTSECRET_PAGE; i++)      // the code only programs blank pages
        CHECK_EQ(PAGE(page)[i], 0xff);
    if (programs == program_fails_at)
        return false;
    size_t n = programs == program_tears_at ? 20 : ROOTSECRET_PAGE;     // power lost partway
    for (size_t i = 0; i < n; i++)
        PAGE(page)[i] &= data[i];
    return true;
}
static bool s_erase(void *ctx) {
    (void)ctx;
    erases++;
    if (erase_fails)
        return false;
    memset(flash, 0xff, sizeof(flash));
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

static const rootsecret_store_t fake_store = { s_read, s_program, s_erase, s_random, NULL };

static void blank(void) {
    memset(flash, 0xff, sizeof(flash));
    programs = erases = 0;
    erase_fails = random_fails = false;
    program_fails_at = program_tears_at = 0;
    next_random = 0x40;
}

// A sector with a good secret (0x40, 0x41, ...) in both copies.
static void made(void) {
    blank();
    CHECK_EQ(rootsecret_ensure(&fake_store, NULL), ROOTSECRET_OK);
    programs = 0;
}

static void check_secret_is(uint8_t first) {
    uint8_t secret[ROOTSECRET_LEN];
    CHECK(rootsecret_read(&fake_store, secret));
    for (unsigned i = 0; i < ROOTSECRET_LEN; i++)
        CHECK_EQ(secret[i], (uint8_t)(first + i));
}

TEST(blank_sector_gets_a_secret_in_both_copies) {
    blank();
    CHECK_EQ(rootsecret_state(&fake_store), ROOTSECRET_NONE);
    bool created = false;
    CHECK_EQ(rootsecret_ensure(&fake_store, &created), ROOTSECRET_OK);
    CHECK(created);
    CHECK_EQ(programs, 2);
    CHECK_EQ(erases, 0);                    // made without erasing anything
    CHECK_MEM(PAGE(0), PAGE(1), ROOTSECRET_PAGE);
    CHECK(!rootsecret_degraded(&fake_store));
    check_secret_is(0x40);
}

// The stored layout, byte for byte: magic "SOSR", version 1, the secret, FNV-1a of the secret,
// the rest of the page erased; the same in pages 0 and 1; the rest of the sector untouched.
// Released firmware has to keep reading this.
TEST(record_layout) {
    made();
    static const uint8_t head[8] = { 'S', 'O', 'S', 'R', 1, 0, 0, 0 };
    for (unsigned copy = 0; copy < 2; copy++) {
        const uint8_t *p = PAGE(copy);
        CHECK_MEM(p, head, sizeof(head));
        for (unsigned i = 0; i < ROOTSECRET_LEN; i++)
            CHECK_EQ(p[8 + i], 0x40 + i);
        uint32_t h = 2166136261u;
        for (unsigned i = 0; i < ROOTSECRET_LEN; i++)
            h = (h ^ p[8 + i]) * 16777619u;
        CHECK_EQ(p[40], h & 0xff);
        CHECK_EQ(p[41], (h >> 8) & 0xff);
        CHECK_EQ(p[42], (h >> 16) & 0xff);
        CHECK_EQ(p[43], h >> 24);
        for (unsigned i = 44; i < ROOTSECRET_PAGE; i++)
            CHECK_EQ(p[i], 0xff);
    }
    for (unsigned i = 2 * ROOTSECRET_PAGE; i < ROOTSECRET_SECTOR; i++)
        CHECK_EQ(flash[i], 0xff);
}

TEST(page_b_is_written_before_page_a) {
    blank();
    program_fails_at = 2;                   // the second program is page A
    CHECK_EQ(rootsecret_ensure(&fake_store, NULL), ROOTSECRET_NONE);
    for (unsigned i = 0; i < ROOTSECRET_PAGE; i++)
        CHECK_EQ(PAGE(0)[i], 0xff);
    CHECK_EQ(PAGE(1)[0], 'S');
}

TEST(an_existing_secret_is_kept) {
    made();
    uint8_t before[ROOTSECRET_SECTOR];
    memcpy(before, flash, sizeof(before));
    bool created = true;
    CHECK_EQ(rootsecret_ensure(&fake_store, &created), ROOTSECRET_OK);
    CHECK(!created);
    CHECK_EQ(programs, 0);
    CHECK_EQ(erases, 0);
    CHECK_MEM(flash, before, sizeof(before));
}

// Making a secret doesn't touch the rest of the sector, whatever is there.
TEST(other_pages_are_not_touched) {
    blank();
    memset(PAGE(2), 0x5a, ROOTSECRET_SECTOR - 2 * ROOTSECRET_PAGE);
    CHECK_EQ(rootsecret_ensure(&fake_store, NULL), ROOTSECRET_OK);
    CHECK_EQ(erases, 0);
    for (unsigned i = 2 * ROOTSECRET_PAGE; i < ROOTSECRET_SECTOR; i++)
        CHECK_EQ(flash[i], 0x5a);
}

// --- one damaged copy: the other is used ---------------------------------------------------------------

TEST(damaged_copy_a_uses_b) {
    made();
    PAGE(0)[20] ^= 1;
    CHECK_EQ(rootsecret_state(&fake_store), ROOTSECRET_OK);
    CHECK(rootsecret_degraded(&fake_store));
    check_secret_is(0x40);
    uint8_t before[ROOTSECRET_SECTOR];
    memcpy(before, flash, sizeof(before));
    CHECK_EQ(rootsecret_ensure(&fake_store, NULL), ROOTSECRET_OK);
    CHECK_EQ(programs, 0);                  // a damaged copy isn't repaired: that needs an erase
    CHECK_EQ(erases, 0);
    CHECK_MEM(flash, before, sizeof(before));
}

TEST(damaged_copy_b_uses_a) {
    made();
    PAGE(1)[40] ^= 0x80;
    CHECK_EQ(rootsecret_ensure(&fake_store, NULL), ROOTSECRET_OK);
    CHECK(rootsecret_degraded(&fake_store));
    CHECK_EQ(programs, 0);
    check_secret_is(0x40);
}

// A record made before there were two copies (page 0 only): the second copy is filled in.
TEST(single_copy_record_gets_its_second_copy) {
    made();
    memset(PAGE(1), 0xff, ROOTSECRET_PAGE);
    CHECK_EQ(rootsecret_state(&fake_store), ROOTSECRET_OK);
    CHECK(rootsecret_degraded(&fake_store));
    bool created = true;
    CHECK_EQ(rootsecret_ensure(&fake_store, &created), ROOTSECRET_OK);
    CHECK(!created);
    CHECK_EQ(programs, 1);
    CHECK_EQ(erases, 0);
    CHECK_MEM(PAGE(0), PAGE(1), ROOTSECRET_PAGE);
    CHECK(!rootsecret_degraded(&fake_store));
    check_secret_is(0x40);
}

TEST(failing_to_fill_the_second_copy_still_works) {
    made();
    memset(PAGE(1), 0xff, ROOTSECRET_PAGE);
    program_fails_at = 1;
    CHECK_EQ(rootsecret_ensure(&fake_store, NULL), ROOTSECRET_OK);
    CHECK(rootsecret_degraded(&fake_store));
    check_secret_is(0x40);
}

// --- what is never replaced automatically ---------------------------------------------------------------

static void check_left_alone(void) {
    uint8_t before[ROOTSECRET_SECTOR], secret[ROOTSECRET_LEN];
    memcpy(before, flash, sizeof(before));
    programs = erases = 0;
    bool created = true;
    CHECK_EQ(rootsecret_state(&fake_store), ROOTSECRET_INVALID);
    CHECK(!rootsecret_degraded(&fake_store));
    CHECK_EQ(rootsecret_ensure(&fake_store, &created), ROOTSECRET_INVALID);
    CHECK(!created);
    CHECK_EQ(programs, 0);
    CHECK_EQ(erases, 0);
    CHECK_MEM(flash, before, sizeof(before));
    memset(secret, 0xaa, sizeof(secret));
    CHECK(!rootsecret_read(&fake_store, secret));
    CHECK(all_zero(secret, sizeof(secret)));
}

TEST(both_copies_damaged_are_left_alone) {
    made();
    PAGE(0)[20] ^= 1;
    PAGE(1)[21] ^= 1;
    check_left_alone();
}

TEST(damaged_a_with_blank_b_is_left_alone) {
    made();
    memset(PAGE(1), 0xff, ROOTSECRET_PAGE);
    PAGE(0)[40] ^= 0x80;
    check_left_alone();
}

TEST(newer_version_is_left_alone) {
    made();
    PAGE(0)[4] = 2;
    PAGE(1)[4] = 2;
    check_left_alone();
    made();
    memset(PAGE(1), 0xff, ROOTSECRET_PAGE);
    PAGE(0)[4] = 2;
    check_left_alone();
}

TEST(two_different_secrets_are_left_alone) {
    made();
    uint8_t other[ROOTSECRET_PAGE];
    memcpy(other, PAGE(0), sizeof(other));
    blank();
    next_random = 0x90;
    CHECK_EQ(rootsecret_ensure(&fake_store, NULL), ROOTSECRET_OK);
    memcpy(PAGE(0), other, sizeof(other));  // both valid, not the same secret
    check_left_alone();
}

TEST(unknown_contents_are_left_alone) {
    blank();
    PAGE(0)[0] = 0;                         // one byte that isn't erased, no magic
    check_left_alone();
    blank();
    PAGE(0)[ROOTSECRET_PAGE - 1] = 0x7f;    // even at the end of the page
    check_left_alone();
    memset(flash, 0, sizeof(flash));
    check_left_alone();
    made();
    PAGE(0)[100] = 0;                       // a record with something after it isn't valid
    PAGE(1)[100] = 0;
    check_left_alone();
}

// Page A blank and page B holding something that isn't what a cut-off write leaves.
TEST(unknown_page_b_is_left_alone) {
    blank();
    PAGE(1)[200] = 0;                       // past where a record ends
    check_left_alone();
    blank();
    PAGE(1)[0] = 'S';                       // looks cut off, but the sector holds something else
    PAGE(5)[0] = 0;
    check_left_alone();
}

// --- failures and power cuts while making it -------------------------------------------------------------

TEST(no_random_numbers_no_secret) {
    blank();
    random_fails = true;
    bool created = true;
    CHECK_EQ(rootsecret_ensure(&fake_store, &created), ROOTSECRET_NONE);
    CHECK(!created);
    CHECK_EQ(programs, 0);
}

TEST(failed_first_program_leaves_none) {
    blank();
    program_fails_at = 1;
    bool created = true;
    CHECK_EQ(rootsecret_ensure(&fake_store, &created), ROOTSECRET_NONE);
    CHECK(!created);
    program_fails_at = 0;                   // the next boot tries again
    CHECK_EQ(rootsecret_ensure(&fake_store, &created), ROOTSECRET_OK);
    CHECK(created);
}

// Power lost while page B was being programmed: nothing was ever used, so the next boot erases
// the partial record and makes a new secret.
TEST(torn_page_b_starts_over) {
    blank();
    program_tears_at = 1;
    bool created = true;
    CHECK_EQ(rootsecret_ensure(&fake_store, &created), ROOTSECRET_NONE);
    CHECK(!created);
    CHECK_EQ(rootsecret_state(&fake_store), ROOTSECRET_NONE);
    uint8_t secret[ROOTSECRET_LEN];
    CHECK(!rootsecret_read(&fake_store, secret));
    program_tears_at = 0;
    CHECK_EQ(rootsecret_ensure(&fake_store, &created), ROOTSECRET_OK);
    CHECK(created);
    CHECK_EQ(erases, 1);
    CHECK_MEM(PAGE(0), PAGE(1), ROOTSECRET_PAGE);
    check_secret_is(0x60);                  // a new secret, not the cut-off one
}

// Page A blank and page B a complete record of a version this firmware doesn't know: by the
// format's rule (page A is written last, nothing is used before) that secret was never used, so
// it's started over like a cut-off write.
TEST(unused_newer_record_in_page_b_starts_over) {
    made();
    memset(PAGE(0), 0xff, ROOTSECRET_PAGE);
    PAGE(1)[4] = 2;
    CHECK_EQ(rootsecret_state(&fake_store), ROOTSECRET_NONE);
    bool created = false;
    CHECK_EQ(rootsecret_ensure(&fake_store, &created), ROOTSECRET_OK);
    CHECK(created);
    CHECK_EQ(erases, 1);
    check_secret_is(0x60);
}

// The write of page A is cut short during the very first boot: B's secret is in use from then
// on, and the caller is told a secret was made.
TEST(first_boot_with_a_torn_page_a_reports_created) {
    blank();
    program_tears_at = 2;
    bool created = false;
    CHECK_EQ(rootsecret_ensure(&fake_store, &created), ROOTSECRET_OK);
    CHECK(created);
    CHECK(rootsecret_degraded(&fake_store));
}

TEST(torn_page_b_and_a_failing_erase) {
    blank();
    program_tears_at = 1;
    CHECK_EQ(rootsecret_ensure(&fake_store, NULL), ROOTSECRET_NONE);
    erase_fails = true;
    CHECK_EQ(rootsecret_ensure(&fake_store, NULL), ROOTSECRET_NONE);
    CHECK_EQ(programs, 1);                  // nothing programmed over the partial record
}

// Power lost between the two pages: B is complete, A blank. The secret wasn't used yet (state
// "none", nothing to read); the next boot finishes with the same secret.
TEST(power_cut_between_the_copies_is_finished) {
    blank();
    program_fails_at = 2;
    CHECK_EQ(rootsecret_ensure(&fake_store, NULL), ROOTSECRET_NONE);
    CHECK_EQ(rootsecret_state(&fake_store), ROOTSECRET_NONE);
    uint8_t secret[ROOTSECRET_LEN];
    memset(secret, 0xaa, sizeof(secret));
    CHECK(!rootsecret_read(&fake_store, secret));
    CHECK(all_zero(secret, sizeof(secret)));
    program_fails_at = 0;
    bool created = true;
    CHECK_EQ(rootsecret_ensure(&fake_store, &created), ROOTSECRET_OK);
    CHECK(!created);                        // finished, not made anew
    CHECK_EQ(erases, 0);
    check_secret_is(0x40);
}

// Power lost while page A was being programmed: A is part of a record, B is complete. B is used.
TEST(torn_page_a_uses_b) {
    blank();
    program_tears_at = 2;
    CHECK_EQ(rootsecret_ensure(&fake_store, NULL), ROOTSECRET_OK);
    CHECK(rootsecret_degraded(&fake_store));
    check_secret_is(0x40);
    program_tears_at = 0;
    CHECK_EQ(rootsecret_ensure(&fake_store, NULL), ROOTSECRET_OK);
    CHECK_EQ(erases, 0);
    check_secret_is(0x40);
}

// --- forget ----------------------------------------------------------------------------------------------

TEST(forget_then_a_new_secret) {
    made();
    CHECK(rootsecret_forget(&fake_store));
    CHECK_EQ(rootsecret_state(&fake_store), ROOTSECRET_NONE);
    uint8_t secret[ROOTSECRET_LEN];
    CHECK(!rootsecret_read(&fake_store, secret));
    CHECK_EQ(rootsecret_ensure(&fake_store, NULL), ROOTSECRET_OK);
    check_secret_is(0x60);
}

TEST(forget_clears_an_invalid_sector) {
    blank();
    memset(flash, 0, sizeof(flash));
    CHECK(rootsecret_forget(&fake_store));
    CHECK_EQ(rootsecret_state(&fake_store), ROOTSECRET_NONE);
}

TEST(failed_forget_says_so) {
    made();
    erase_fails = true;
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
    RUN(init_again_changes_nothing);
    RUN(derive_fails_before_init);
    RUN(backend_without_a_lock);
    RUN(backend_without_a_root);
    RUN(blank_sector_gets_a_secret_in_both_copies);
    RUN(record_layout);
    RUN(page_b_is_written_before_page_a);
    RUN(an_existing_secret_is_kept);
    RUN(other_pages_are_not_touched);
    RUN(damaged_copy_a_uses_b);
    RUN(damaged_copy_b_uses_a);
    RUN(single_copy_record_gets_its_second_copy);
    RUN(failing_to_fill_the_second_copy_still_works);
    RUN(both_copies_damaged_are_left_alone);
    RUN(damaged_a_with_blank_b_is_left_alone);
    RUN(newer_version_is_left_alone);
    RUN(two_different_secrets_are_left_alone);
    RUN(unknown_contents_are_left_alone);
    RUN(unknown_page_b_is_left_alone);
    RUN(no_random_numbers_no_secret);
    RUN(failed_first_program_leaves_none);
    RUN(torn_page_b_starts_over);
    RUN(unused_newer_record_in_page_b_starts_over);
    RUN(first_boot_with_a_torn_page_a_reports_created);
    RUN(torn_page_b_and_a_failing_erase);
    RUN(power_cut_between_the_copies_is_finished);
    RUN(torn_page_a_uses_b);
    RUN(forget_then_a_new_secret);
    RUN(forget_clears_an_invalid_sector);
    RUN(failed_forget_says_so);
)
