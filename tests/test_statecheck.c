// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/statecheck.c, the pure half of the checks of stored state
// (docs/design/health.md §9.3): each check's judging from made-up inputs, one test per status
// path; the OTP row ECC against picotool's code and under damage; the dev key hashes against
// keys/*.pem.

#include <stdio.h>
#include <string.h>

#include "core/rootsecret.h"
#include "core/statecheck.h"
#include "test.h"

static health_result_t r;

// A text of HEALTH_TEXT_MAX - 1 characters may have been cut: every text tested must be shorter.
static bool says(const char *part) {
    CHECK(strlen(r.text) < HEALTH_TEXT_MAX - 1);
    return strstr(r.text, part) != NULL;
}

// --- config ----------------------------------------------------------------------------------

static statecheck_config_t cfg(config_slot_kind_t a, uint32_t seq_a, config_slot_kind_t b,
                               uint32_t seq_b) {
    statecheck_config_t c = {true, {{a, 2, 2292, seq_a}, {b, 2, 2292, seq_b}}};
    return c;
}

TEST(config_pass_two_sealed) {
    statecheck_config_t c = cfg(CONFIG_SLOT_SEALED, 27, CONFIG_SLOT_SEALED, 28);
    statecheck_judge_config(&c, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(!strcmp(r.text, "rev 28 sealed, in use (B); A rev 27 sealed"));
}

TEST(config_pass_other_empty) {
    statecheck_config_t c = cfg(CONFIG_SLOT_SEALED, 5, CONFIG_SLOT_EMPTY, 0xffffffffu);
    statecheck_judge_config(&c, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(!strcmp(r.text, "rev 5 sealed, in use (A); B empty"));
}

TEST(config_warn_plain_not_sealed) {
    statecheck_config_t c = cfg(CONFIG_SLOT_PLAIN, 3, CONFIG_SLOT_EMPTY, 0);
    statecheck_judge_config(&c, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(says("plain rev 3 (A) in use: not sealed yet"));
}

TEST(config_warn_plain_after_sealed) {
    // The plain one is in use whatever the numbers (a downgrade starts again at 1).
    statecheck_config_t c = cfg(CONFIG_SLOT_SEALED, 40, CONFIG_SLOT_PLAIN, 1);
    statecheck_judge_config(&c, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(!strcmp(r.text, "plain rev 1 (B) in use, written after sealing (downgrade?)"));
}

TEST(config_warn_other_damaged) {
    config_slot_kind_t bad[] = {CONFIG_SLOT_JUNK, CONFIG_SLOT_BAD_HEADER, CONFIG_SLOT_BAD_CRC,
                                CONFIG_SLOT_INVALID};
    for (unsigned i = 0; i < 4; i++) {
        statecheck_config_t c = cfg(bad[i], 27, CONFIG_SLOT_SEALED, 28);
        statecheck_judge_config(&c, &r);
        CHECK_EQ(r.status, HEALTH_WARN);
        CHECK(says("no fallback"));
        CHECK(says(config_slot_kind_name(bad[i])));
    }
    statecheck_config_t c = cfg(CONFIG_SLOT_BAD_CRC, 27, CONFIG_SLOT_SEALED, 28);
    statecheck_judge_config(&c, &r);
    CHECK(!strcmp(r.text, "rev 28 sealed, in use (B); A bad CRC: no fallback"));
}

TEST(config_warn_other_unopened) {
    statecheck_config_t c = cfg(CONFIG_SLOT_SEALED, 28, CONFIG_SLOT_UNOPENED, 27);
    statecheck_judge_config(&c, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(says("B doesn't open"));
}

TEST(config_warn_refused_left) {
    statecheck_config_t c = cfg(CONFIG_SLOT_REFUSED, 1, CONFIG_SLOT_SEALED, 28);
    statecheck_judge_config(&c, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(says("refused plain left (console-only)"));
}

TEST(config_warn_only_refused) {
    statecheck_config_t c = cfg(CONFIG_SLOT_REFUSED, 1, CONFIG_SLOT_EMPTY, 0);
    statecheck_judge_config(&c, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(says("running unconfigured"));
}

TEST(config_fail_unopened) {
    statecheck_config_t c = cfg(CONFIG_SLOT_UNOPENED, 3, CONFIG_SLOT_EMPTY, 0);
    statecheck_judge_config(&c, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(says("doesn't open; running unconfigured"));
}

TEST(config_fail_damaged) {
    statecheck_config_t c = cfg(CONFIG_SLOT_EMPTY, 0, CONFIG_SLOT_BAD_CRC, 9);
    statecheck_judge_config(&c, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(!strcmp(r.text, "slot B bad CRC, no usable record; running on defaults"));
}

TEST(config_fail_not_persistent) {
    statecheck_config_t c = cfg(CONFIG_SLOT_SEALED, 3, CONFIG_SLOT_EMPTY, 0);
    c.persistent = false;
    statecheck_judge_config(&c, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(says("settings aren't saved"));
}

TEST(config_skip_nothing_saved) {
    statecheck_config_t c = cfg(CONFIG_SLOT_EMPTY, 0, CONFIG_SLOT_EMPTY, 0);
    statecheck_judge_config(&c, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
}

// --- rootsecret ------------------------------------------------------------------------------

static statecheck_root_t otp_root(void) {
    statecheck_root_t x;
    memset(&x, 0, sizeof(x));
    x.source = ROOTSRC_OTP;
    x.found = ROOTOTP_FOUND;
    x.page = 5;
    x.secret_ok = x.locks_ok = x.soft_locked = true;
    return x;
}

static statecheck_root_t flash_root(void) {
    statecheck_root_t x;
    memset(&x, 0, sizeof(x));
    x.source = ROOTSRC_FLASH;
    x.found = ROOTOTP_NONE_YET;
    x.page = 4;
    x.flash_usable = true;
    x.flash_state = ROOTSECRET_OK;
    return x;
}

TEST(root_pass_otp) {
    statecheck_root_t x = otp_root();
    statecheck_judge_root(&x, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(!strcmp(r.text, "OTP page 5, locked for this boot; list pages locked"));
}

TEST(root_warn_otp_not_soft_locked) {
    statecheck_root_t x = otp_root();
    x.soft_locked = false;
    statecheck_judge_root(&x, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(says("not locked for this boot"));
}

TEST(root_warn_otp_list_writable) {
    statecheck_root_t x = otp_root();
    x.locks_ok = false;
    x.open_page = 4;
    statecheck_judge_root(&x, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(says("page 4 writable from the bootloader"));
}

TEST(root_pass_flash) {
    statecheck_root_t x = flash_root();
    statecheck_judge_root(&x, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(says("both copies intact"));
}

TEST(root_warn_flash_degraded_and_spent) {
    statecheck_root_t x = flash_root();
    x.flash_degraded = true;
    statecheck_judge_root(&x, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(says("one copy damaged"));
    x = flash_root();
    x.found = ROOTOTP_EXHAUSTED;
    statecheck_judge_root(&x, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(says("spent"));
}

TEST(root_fail_flash) {
    statecheck_root_t x = flash_root();
    x.flash_state = ROOTSECRET_INVALID;
    statecheck_judge_root(&x, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(says("unusable"));
    x.flash_state = ROOTSECRET_NONE;
    statecheck_judge_root(&x, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(says("making it failed"));
    x.flash_usable = false;
    statecheck_judge_root(&x, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(says("overlaps"));
    x = flash_root();
    x.console_only = true;
    statecheck_judge_root(&x, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(says("a bug"));
}

TEST(root_fail_none_each_reason) {
    statecheck_root_t x = otp_root();
    x.source = ROOTSRC_NONE;
    x.locks_ok = false;
    x.open_page = 4;
    statecheck_judge_root(&x, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(says("page 4 writable"));
    x.secret_ok = false;
    statecheck_judge_root(&x, &r);
    CHECK(says("fails its check"));
    static const roototp_find_t found[] = {ROOTOTP_BAD_MARKER, ROOTOTP_CANT_READ,
                                           ROOTOTP_EXHAUSTED, ROOTOTP_NONE_YET};
    static const char *const why[] = {"marker", "can't be read", "spent", "nothing in OTP"};
    for (unsigned i = 0; i < 4; i++) {
        x.found = found[i];
        statecheck_judge_root(&x, &r);
        CHECK_EQ(r.status, HEALTH_FAIL);
        CHECK(says(why[i]));
        CHECK(says("no keys"));
    }
}

// --- otp: the row ECC ------------------------------------------------------------------------

// picotool's otp_calculate_ecc() (main.cpp), transcribed with its binary masks, to compare.
static uint32_t even_parity(uint32_t input) {
    return (uint32_t)__builtin_popcount(input) & 1;
}
static uint32_t picotool_ecc(uint16_t x) {
    uint32_t p0 = even_parity(x & 0b1010110101011011);
    uint32_t p1 = even_parity(x & 0b0011011001101101);
    uint32_t p2 = even_parity(x & 0b1100011110001110);
    uint32_t p3 = even_parity(x & 0b0000011111110000);
    uint32_t p4 = even_parity(x & 0b1111100000000000);
    uint32_t p5 = even_parity(x) ^ p0 ^ p1 ^ p2 ^ p3 ^ p4;
    uint32_t p = p0 | (p1 << 1) | (p2 << 2) | (p3 << 3) | (p4 << 4) | (p5 << 5);
    return x | (p << 16);
}

TEST(ecc_matches_picotool) {
    for (uint32_t x = 0; x <= 0xffff; x++)
        if (statecheck_otp_ecc((uint16_t)x) != picotool_ecc((uint16_t)x)) {
            CHECK_EQ(statecheck_otp_ecc((uint16_t)x), picotool_ecc((uint16_t)x));
            break;
        }
    CHECK_EQ(statecheck_otp_ecc(0), 0);
}

TEST(ecc_clean_rows_agree_with_the_corrected_alias) {
    static const uint16_t data[] = {0x0000, 0x1234, 0xa55a, 0xffff, 0x8001};
    for (unsigned i = 0; i < 5; i++) {
        uint32_t raw = statecheck_otp_ecc(data[i]);
        CHECK_EQ(statecheck_otp_row(raw, data[i]), STATECHECK_ROW_OK);
        // Stored inverted with both bit-repair bits set: the same row.
        CHECK_EQ(statecheck_otp_row(raw ^ 0xffffffu, data[i]), STATECHECK_ROW_OK);
    }
}

TEST(ecc_damage_is_seen) {
    uint16_t data = 0x1234;
    uint32_t raw = statecheck_otp_ecc(data);
    // Every one-bit flip of the 22 bits, and every two-bit flip (SECDED: distance 4).
    unsigned seen1 = 0, seen2 = 0, pairs = 0;
    for (unsigned a = 0; a < 22; a++) {
        if (statecheck_otp_row(raw ^ (1u << a), data) == STATECHECK_ROW_ECC_MISMATCH)
            seen1++;
        for (unsigned b = a + 1; b < 22; b++, pairs++)
            if (statecheck_otp_row(raw ^ (1u << a) ^ (1u << b), data) ==
                STATECHECK_ROW_ECC_MISMATCH)
                seen2++;
    }
    CHECK_EQ(seen1, 22);
    CHECK_EQ(seen2, pairs);
    // The corrected alias returning other data than the row holds (the chip corrected it).
    CHECK_EQ(statecheck_otp_row(raw, (uint16_t)(data ^ 4)), STATECHECK_ROW_CORRECTED);
    CHECK_EQ(statecheck_otp_row(STATECHECK_UNREADABLE, 0xffff), STATECHECK_ROW_UNREADABLE);
}

// --- otp: the judging --------------------------------------------------------------------------

// A board like dev-1: secure boot on, debug on, slots 0 and 1 the two dev keys, KEY_VALID = 3,
// signed with dev key 1.
static statecheck_otp_t dev1(void) {
    statecheck_otp_t o;
    memset(&o, 0, sizeof(o));
    o.latched = STATECHECK_SECURE_BOOT;
    for (unsigned i = 0; i < STATECHECK_CRIT1_COPIES; i++)
        o.crit1[i] = STATECHECK_SECURE_BOOT;
    for (unsigned i = 0; i < STATECHECK_FLAGS1_COPIES; i++)
        o.flags1[i] = 0x3;
    for (unsigned s = 0; s < 2; s++)
        for (unsigned k = 0; k < BOOTKEY_SLOT_ROWS; k++) {
            const uint8_t *h = statecheck_dev_key_hash[s];
            uint16_t d = (uint16_t)(h[2 * k] | h[2 * k + 1] << 8);
            o.key_ecc[s * BOOTKEY_SLOT_ROWS + k] = d;
            o.key_raw[s * BOOTKEY_SLOT_ROWS + k] = statecheck_otp_ecc(d);
        }
    o.image_signed = true;
    memcpy(o.image_hash, statecheck_dev_key_hash[0], BOOTKEY_HASH_LEN);
    return o;
}

TEST(otp_pass_dev1_debug_build) {
    statecheck_otp_t o = dev1();
    statecheck_judge_otp(&o, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(!strcmp(r.text, "secure boot on, debug on; valid 0=dev1 1=dev2; image key in 0"));
}

TEST(otp_warn_dev_key_release_build) {
    statecheck_otp_t o = dev1();
    o.release = true;
    statecheck_judge_otp(&o, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(!strcmp(r.text, "valid slot 0 holds public dev key 1 (release build)"));
}

TEST(otp_warn_unreadable_row) {
    statecheck_otp_t o = dev1();
    o.key_raw[5] = STATECHECK_UNREADABLE;
    statecheck_judge_otp(&o, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(!strcmp(r.text, "row 0x085 unreadable (locked or damaged)"));
    o = dev1();
    o.crit1[7] = STATECHECK_UNREADABLE;
    statecheck_judge_otp(&o, &r);
    CHECK(says("row 0x047"));
    o = dev1();
    o.flags1[0] = STATECHECK_UNREADABLE;
    statecheck_judge_otp(&o, &r);
    CHECK(says("row 0x04b"));
}

TEST(otp_warn_ecc) {
    statecheck_otp_t o = dev1();
    o.key_raw[17] ^= 1u << 18;              // a parity bit of slot 1's second row
    statecheck_judge_otp(&o, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(says("boot key slot 1 row 0x091: ECC mismatch"));
    o = dev1();
    // The corrected alias differs from the stored data, in slot 1: not the image's key, whose
    // changed hash would be the FAIL that comes first.
    o.key_ecc[19] ^= 1;
    statecheck_judge_otp(&o, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(says("row 0x093: corrected"));
}

TEST(otp_warn_crit1) {
    statecheck_otp_t o = dev1();
    o.crit1[2] |= STATECHECK_DEBUG_DISABLE; // one copy only: outvoted, but they disagree
    statecheck_judge_otp(&o, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(says("CRIT1 copies disagree on bits 0x04"));
    o = dev1();
    for (unsigned i = 0; i < STATECHECK_CRIT1_COPIES; i++)
        o.crit1[i] |= STATECHECK_DEBUG_DISABLE;     // written since the reset
    statecheck_judge_otp(&o, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(says("CRIT1 rows 0x05 vs latched 0x01"));
}

TEST(otp_warn_flags1_disagree) {
    statecheck_otp_t o = dev1();
    o.flags1[1] |= 0x4;                     // slot 2 valid in one copy
    statecheck_judge_otp(&o, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(says("BOOT_FLAGS1 copies disagree on bits 0x004"));
}

TEST(otp_fail_image_key_not_valid) {
    statecheck_otp_t o = dev1();
    for (unsigned i = 0; i < STATECHECK_FLAGS1_COPIES; i++)
        o.flags1[i] = 0x3 | 0x100;          // slot 0 marked invalid
    statecheck_judge_otp(&o, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(!strcmp(r.text, "image key in no valid slot: no boot after a reset; running on"));
    // With secure boot off it is only reported.
    o.latched = 0;
    for (unsigned i = 0; i < STATECHECK_CRIT1_COPIES; i++)
        o.crit1[i] = 0;
    statecheck_judge_otp(&o, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(says("secure boot off"));
    CHECK(says("image key in none"));
}

// The FAIL isn't hidden by a WARN: a damaged key row and split CRIT1 copies beside a revoked
// image key still give the FAIL (Reviewer on #254).
TEST(otp_fail_before_warns) {
    statecheck_otp_t o = dev1();
    for (unsigned i = 0; i < STATECHECK_FLAGS1_COPIES; i++)
        o.flags1[i] = 0x3 | 0x100;          // slot 0, the image's key, marked invalid
    o.key_raw[17] ^= 1u << 18;              // and slot 1's second row damaged
    o.crit1[2] |= STATECHECK_DEBUG_DISABLE; // and one CRIT1 copy split
    statecheck_judge_otp(&o, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(says("image key in no valid slot"));
}

TEST(otp_skip_open_board) {
    statecheck_otp_t o;
    memset(&o, 0, sizeof(o));
    statecheck_judge_otp(&o, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
}

// The constants against the files they come from, when openssl is there to read them.
TEST(dev_key_hashes_match_the_pem_files) {
    for (unsigned i = 0; i < STATECHECK_DEV_KEYS; i++) {
        char cmd[512], hex[80] = "";
        snprintf(cmd, sizeof(cmd),
                 "openssl ec -in '%s/dev-INSECURE-%u.pem' -pubout -outform DER 2>/dev/null | "
                 "tail -c 64 | sha256sum 2>/dev/null", SOS_KEYS_DIR, i + 1);
        FILE *p = popen(cmd, "r");
        if (!p)
            return;
        bool got = fgets(hex, sizeof(hex), p) != NULL;
        pclose(p);
        if (!got || strlen(hex) < 64 || !strncmp(hex, "e3b0c442", 8))    // no openssl: empty input
        {
            printf("dev key %u: openssl not available, not compared\n", i + 1);
            continue;
        }
        char want[65];
        for (unsigned k = 0; k < BOOTKEY_HASH_LEN; k++)
            snprintf(want + 2 * k, 3, "%02x", statecheck_dev_key_hash[i][k]);
        CHECK(!strncmp(hex, want, 64));
        CHECK_EQ(statecheck_dev_key(statecheck_dev_key_hash[i]), i + 1);
    }
    uint8_t other[BOOTKEY_HASH_LEN] = {1};
    CHECK_EQ(statecheck_dev_key(other), 0);
}

// --- failcount -------------------------------------------------------------------------------

static statecheck_failcount_t fc(failcount_sector_t st, unsigned attempts, unsigned cleared,
                                 unsigned count) {
    statecheck_failcount_t f;
    memset(&f, 0, sizeof(f));
    f.usable = true;
    f.info.state = st;
    f.info.attempts = attempts;
    f.info.cleared = cleared;
    f.info.cleared_over = cleared > attempts;
    f.count = count;
    return f;
}

TEST(failcount_pass) {
    statecheck_failcount_t f = fc(FAILCOUNT_SECTOR_MARKED, 37, 37, 0);
    statecheck_judge_failcount(&f, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(!strcmp(r.text, "0 wrong passwords; 37 of 14336 checks used"));
    f = fc(FAILCOUNT_SECTOR_ERASED, 0, 0, 0);
    statecheck_judge_failcount(&f, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(says("no wrong passwords recorded"));
}

TEST(failcount_warn_count) {
    statecheck_failcount_t f = fc(FAILCOUNT_SECTOR_MARKED, 40, 37, 3);
    statecheck_judge_failcount(&f, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(!strcmp(r.text, "3 wrong passwords since the last right one"));
    f = fc(FAILCOUNT_SECTOR_MARKED, 38, 37, 1);
    statecheck_judge_failcount(&f, &r);
    CHECK(!strcmp(r.text, "1 wrong password since the last right one"));
}

TEST(failcount_warn_ram_only) {
    statecheck_failcount_t f = fc(FAILCOUNT_SECTOR_MARKED, 38, 37, 3);
    statecheck_judge_failcount(&f, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(says("2 counted in RAM only"));
}

TEST(failcount_warn_no_marker) {
    statecheck_failcount_t f = fc(FAILCOUNT_SECTOR_OTHER, 0, 0, 0);
    statecheck_judge_failcount(&f, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(says("no marker"));
}

TEST(failcount_warn_inconsistent) {
    statecheck_failcount_t f = fc(FAILCOUNT_SECTOR_MARKED, 5, 7, 0);
    statecheck_judge_failcount(&f, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(says("tracks inconsistent (attempts 5, cleared 7): read as 0"));
    f = fc(FAILCOUNT_SECTOR_MARKED, 5, 5, 0);
    f.info.stray = true;
    statecheck_judge_failcount(&f, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
}

TEST(failcount_fail_unusable) {
    statecheck_failcount_t f = fc(FAILCOUNT_SECTOR_NONE, 0, 0, 2);
    f.usable = false;
    statecheck_judge_failcount(&f, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(says("RAM only"));
}

// --- hostkey ---------------------------------------------------------------------------------

TEST(hostkey_paths) {
    statecheck_hostkey_t h = {true, true, false, false, true, "SHA256:abcdef", false};
    statecheck_judge_hostkey(&h, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(!strcmp(r.text, "sealed, as loaded; SHA256:abcdef"));
    h.unsealed = true;
    statecheck_judge_hostkey(&h, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(says("unsealed"));
    h.page_matches = false;
    statecheck_judge_hostkey(&h, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(says("sshd serves the key in RAM"));
    h.erased = true;                        // `hostkey_erase()`: on purpose, so not the FAIL
    statecheck_judge_hostkey(&h, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(!strcmp(r.text, "erased: new key at next boot; sshd serves the old one"));
    statecheck_hostkey_t none = {true, false, true, false, false, NULL, false};
    statecheck_judge_hostkey(&none, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(says("sshd isn't running"));
    none.gave_up = false;
    statecheck_judge_hostkey(&none, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
    none.have_owner = false;
    statecheck_judge_hostkey(&none, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
    CHECK(says("not set up"));
    statecheck_hostkey_t nosshd = {true, true, false, false, true, NULL, false};
    statecheck_judge_hostkey(&nosshd, &r);
    CHECK(says("sshd not started"));
}

// --- card ------------------------------------------------------------------------------------

static statecheck_card_t card(void) {
    statecheck_card_t c;
    memset(&c, 0, sizeof(c));
    c.mounted = true;
    c.fs = "exFAT";
    c.encrypted = true;
    c.var_log = STATECHECK_PATH_DIR;
    c.certs = STATECHECK_PATH_MISSING;
    return c;
}

TEST(card_pass_dev1) {
    statecheck_card_t c = card();
    statecheck_judge_card(&c, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(!strcmp(r.text, "exFAT encrypted; /var/log ok; no /certs: built-in roots only"));
    c.certs = STATECHECK_PATH_DIR;
    statecheck_judge_card(&c, &r);
    CHECK(says("unapproved roots: built-in only"));
    c.approved = true;
    c.roots = STATECHECK_ROOTS_MATCH;
    statecheck_judge_card(&c, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(says("matches its approval"));
}

TEST(card_warn_roots) {
    statecheck_card_t c = card();
    c.certs = STATECHECK_PATH_DIR;
    c.approved = true;
    static const statecheck_roots_t roots[] = {STATECHECK_ROOTS_CHANGED, STATECHECK_ROOTS_MISSING,
                                               STATECHECK_ROOTS_TOO_BIG,
                                               STATECHECK_ROOTS_UNREADABLE};
    for (unsigned i = 0; i < 4; i++) {
        c.roots = roots[i];
        statecheck_judge_card(&c, &r);
        CHECK_EQ(r.status, HEALTH_WARN);
        CHECK(says("built-in roots only"));
    }
}

TEST(card_warn_dirs) {
    statecheck_card_t c = card();
    c.var_log = STATECHECK_PATH_MISSING;
    statecheck_judge_card(&c, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    c.var_log = STATECHECK_PATH_FILE;
    statecheck_judge_card(&c, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(says("rm it on the console"));
    c = card();
    c.certs = STATECHECK_PATH_FILE;
    statecheck_judge_card(&c, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(!strcmp(r.text, "/certs is a file: remove it with rm on the console"));
    c = card();
    c.certs = STATECHECK_PATH_ERROR;
    statecheck_judge_card(&c, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
}

TEST(card_warn_locked) {
    statecheck_card_t c = card();
    c.mounted = false;
    c.locked = true;
    statecheck_judge_card(&c, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(says("doesn't open with this board's key"));
}

TEST(card_skip) {
    statecheck_card_t c = card();
    c.yield = "sd check running";
    statecheck_judge_card(&c, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
    CHECK(!strcmp(r.text, "sd check running"));
    c = card();
    c.mounted = false;
    statecheck_judge_card(&c, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
    CHECK(says("no card mounted"));
}

// --- health state-dump's wrapping ---------------------------------------------------------------

static char wrapped[8][256];
static unsigned pieces;

static void collect(void *arg, const char *piece) {
    (void)arg;
    if (pieces < 8)
        snprintf(wrapped[pieces], sizeof(wrapped[0]), "%s", piece);
    pieces++;
}

TEST(wrap_lines) {
    pieces = 0;
    statecheck_wrap("short line", 79, collect, NULL);
    CHECK_EQ(pieces, 1);
    CHECK(!strcmp(wrapped[0], "short line"));

    // The root secret's line on dev-1, which the 112-byte buffer used to cut.
    const char *root = "rootsecret: source OTP; OTP: found, page 5, secret ok 1, list locked 1 "
                       "(open page 0), soft lock 1; console-only 0";
    pieces = 0;
    statecheck_wrap(root, 79, collect, NULL);
    CHECK_EQ(pieces, 2);
    CHECK(!strcmp(wrapped[0], "rootsecret: source OTP; OTP: found, page 5, secret ok 1,"));
    CHECK(!strcmp(wrapped[1], "  list locked 1 (open page 0), soft lock 1; console-only 0"));

    // Every piece within the width, nothing lost: the pieces put back together are the line.
    const char *fc = "failcount: usable 1, sector marker, attempts 3, cleared 3, stray 0, cleared "
                     "over 0; count (RAM included) 0 of capacity 14336; and a little more, to "
                     "need a third line here, with words";
    pieces = 0;
    statecheck_wrap(fc, 50, collect, NULL);
    CHECK(pieces >= 4 && pieces <= 8);
    char back[512] = "";
    for (unsigned i = 0; i < pieces && i < 8; i++) {
        CHECK(strlen(wrapped[i]) <= 50);
        if (i)
            CHECK(!strncmp(wrapped[i], "  ", 2));
        strcat(back, i ? wrapped[i] + 2 : wrapped[i]);
        if (i + 1 < pieces)
            strcat(back, " ");
    }
    CHECK(!strcmp(back, fc));

    // No separator to break at: cut at the width.
    char longword[200];
    memset(longword, 'x', sizeof(longword) - 1);
    longword[sizeof(longword) - 1] = '\0';
    pieces = 0;
    statecheck_wrap(longword, 79, collect, NULL);
    CHECK_EQ(pieces, 3);
    CHECK_EQ(strlen(wrapped[0]), 79);
    CHECK_EQ(strlen(wrapped[1]), 79);
    CHECK_EQ(strlen(wrapped[2]), 2 + 199 - 79 - 77);
}

TEST_MAIN(
    RUN(config_pass_two_sealed);
    RUN(config_pass_other_empty);
    RUN(config_warn_plain_not_sealed);
    RUN(config_warn_plain_after_sealed);
    RUN(config_warn_other_damaged);
    RUN(config_warn_other_unopened);
    RUN(config_warn_refused_left);
    RUN(config_warn_only_refused);
    RUN(config_fail_unopened);
    RUN(config_fail_damaged);
    RUN(config_fail_not_persistent);
    RUN(config_skip_nothing_saved);
    RUN(root_pass_otp);
    RUN(root_warn_otp_not_soft_locked);
    RUN(root_warn_otp_list_writable);
    RUN(root_pass_flash);
    RUN(root_warn_flash_degraded_and_spent);
    RUN(root_fail_flash);
    RUN(root_fail_none_each_reason);
    RUN(ecc_matches_picotool);
    RUN(ecc_clean_rows_agree_with_the_corrected_alias);
    RUN(ecc_damage_is_seen);
    RUN(otp_pass_dev1_debug_build);
    RUN(otp_warn_dev_key_release_build);
    RUN(otp_warn_unreadable_row);
    RUN(otp_warn_ecc);
    RUN(otp_warn_crit1);
    RUN(otp_warn_flags1_disagree);
    RUN(otp_fail_image_key_not_valid);
    RUN(otp_fail_before_warns);
    RUN(otp_skip_open_board);
    RUN(dev_key_hashes_match_the_pem_files);
    RUN(failcount_pass);
    RUN(failcount_warn_count);
    RUN(failcount_warn_ram_only);
    RUN(failcount_warn_no_marker);
    RUN(failcount_warn_inconsistent);
    RUN(failcount_fail_unusable);
    RUN(hostkey_paths);
    RUN(card_pass_dev1);
    RUN(card_warn_roots);
    RUN(card_warn_dirs);
    RUN(card_warn_locked);
    RUN(card_skip);
    RUN(wrap_lines);
)
