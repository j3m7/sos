// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/fullcheck.c, the pure half of phase 5's checks (docs/design/health.md
// §9.7): `ram`, `image` and `sd-check` judged from made-up inputs, one test per status path at
// least, and the walk over what the image's hash covers.

#include <stdio.h>
#include <string.h>

#include "core/fullcheck.h"
#include "test.h"

static health_result_t r;

// A text of HEALTH_TEXT_MAX - 1 characters may have been cut: every text tested must be shorter.
static bool is(const char *text) {
    CHECK(strlen(r.text) < HEALTH_TEXT_MAX - 1);
    if (strcmp(r.text, text))
        fprintf(stderr, "  text: \"%s\", expected \"%s\"\n", r.text, text);
    return !strcmp(r.text, text);
}

// --- ram -----------------------------------------------------------------------------------------

TEST(ram_pass) {
    fullcheck_ram_t in = {.heap_bytes = 184 * 1024, .free_before = 280 * 1024,
                          .scratch_tested = true, .ms = 123};
    fullcheck_judge_ram(&in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(is("184 KB of free heap + 4 KB SCRATCH_Y in 0.12 s: no faults"));
    in.scratch_tested = false;
    in.scratch_why = "SCRATCH_Y in use";
    in.ms = 110;
    fullcheck_judge_ram(&in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(is("184 KB free heap in 0.11 s; SCRATCH_Y in use: not tested"));
    in.heap_bytes = 999 * 1024;             // the longest a figure gets, with the longest reason
    in.ms = 9999;
    in.scratch_why = "SCRATCH_Y DMA 15 busy";
    fullcheck_judge_ram(&in, &r);
    CHECK(strlen(r.text) < HEALTH_TEXT_MAX - 1);
}

TEST(ram_scratch_only_and_skip) {
    fullcheck_ram_t in = {.free_before = 80 * 1024, .scratch_tested = true, .ms = 3};
    fullcheck_judge_ram(&in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(is("free heap 80 KB, under 96 KB: SCRATCH_Y only, no faults"));
    in.scratch_tested = false;
    in.scratch_why = "SCRATCH_Y in use";
    fullcheck_judge_ram(&in, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
    CHECK(is("free heap 80 KB, under the 96 KB kept free: not tested"));
}

TEST(ram_warn_and_fail) {
    fullcheck_ram_t in = {.heap_bytes = 4096, .fault = FULLCHECK_RAM_ONCE, .addr = 0x2004a1f0,
                          .wrote = 0, .read = 0x400};
    fullcheck_judge_ram(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("0x2004a1f0 read 00000400 for 00000000 once, not again"));
    in.fault = FULLCHECK_RAM_TWICE;
    fullcheck_judge_ram(&in, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(is("RAM 0x2004a1f0 bad: power-cycle; keys, seals off until reset"));
}

// --- image ---------------------------------------------------------------------------------------

static const uint8_t digest[32] = {
    0x43, 0xfe, 0x81, 0x26, 0xfb, 0x07, 0xaf, 0x18, 0xdf, 0x01, 0x5e, 0xc1, 0xb8, 0xd6, 0xea, 0xca,
    0x42, 0xfa, 0xc9, 0x58, 0x69, 0x18, 0x33, 0x35, 0x68, 0x67, 0x41, 0x7b, 0xfe, 0x82, 0x09, 0x24,
};

static bootkey_hash_t hashed(bool signed_image) {
    bootkey_hash_t h;
    memset(&h, 0, sizeof(h));
    h.value_words = 8;
    for (unsigned i = 0; i < 8; i++)
        h.value[i] = (uint32_t)digest[4 * i] | (uint32_t)digest[4 * i + 1] << 8 |
                     (uint32_t)digest[4 * i + 2] << 16 | (uint32_t)digest[4 * i + 3] << 24;
    h.signed_image = signed_image;
    return h;
}

TEST(image_pass) {
    bootkey_hash_t boot = hashed(true), now = hashed(true);
    fullcheck_image_t in = {BOOTKEY_HASH_OK, BOOTKEY_HASH_OK, &boot, &now, digest, 1248860, 451};
    fullcheck_judge_image(&in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(is("signed, sha256 43fe8126fb07af18 as booted, 1.2 MB in 0.45 s"));
    now.signed_image = false;
    fullcheck_judge_image(&in, &r);
    CHECK(is("hashed, sha256 43fe8126fb07af18 as booted, 1.2 MB in 0.45 s"));
    boot.value_words = now.value_words = 1;         // a short HASH_VALUE compares its words only
    fullcheck_judge_image(&in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
}

TEST(image_body_changed) {
    bootkey_hash_t boot = hashed(true), now = hashed(true);
    uint8_t other[32];
    memcpy(other, digest, sizeof(other));
    other[31] ^= 1;
    fullcheck_image_t in = {BOOTKEY_HASH_OK, BOOTKEY_HASH_OK, &boot, &now, other, 1248860, 451};
    fullcheck_judge_image(&in, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(is("flash image changed: reflash; keys, seals off until reset"));
    in.computed = NULL;
    fullcheck_judge_image(&in, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
}

// A changed hash item is reported as such, even when the body was changed to match it.
TEST(image_item_changed) {
    bootkey_hash_t boot = hashed(true), now = hashed(true);
    now.value[2] ^= 0x100;
    fullcheck_image_t in = {BOOTKEY_HASH_OK, BOOTKEY_HASH_OK, &boot, &now, digest, 1, 1};
    fullcheck_judge_image(&in, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(is("image hash item changed: reflash; keys, seals off until reset"));
    now = hashed(true);
    in.now = BOOTKEY_HASH_UNKNOWN;                  // damaged since boot
    fullcheck_judge_image(&in, &r);
    CHECK(is("image hash item changed: reflash; keys, seals off until reset"));
    in.boot = BOOTKEY_HASH_NONE;                    // a hash appeared after boot
    in.now = BOOTKEY_HASH_OK;
    fullcheck_judge_image(&in, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
}

TEST(image_skip_and_warn) {
    fullcheck_image_t in = {BOOTKEY_HASH_NONE, BOOTKEY_HASH_NONE, NULL, NULL, NULL, 0, 0};
    fullcheck_judge_image(&in, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
    CHECK(is("image has no hash (built before phase 5?): not checked"));
    in.boot = in.now = BOOTKEY_HASH_UNKNOWN;
    fullcheck_judge_image(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("image's hash block not understood: not checked"));
    in.boot = in.now = BOOTKEY_HASH_NO_BLOCKS;
    fullcheck_judge_image(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
}

// The pieces cover each entry's stored bytes in order, a zero fill as its size word, then the
// block's words, none longer than asked.
TEST(hash_pieces) {
    bootkey_hash_t h;
    memset(&h, 0, sizeof(h));
    h.loads = 3;
    h.load[0] = (bootkey_load_t){0, 40000};
    h.load[1] = (bootkey_load_t){BOOTKEY_ZERO_FILL, 0x400};
    h.load[2] = (bootkey_load_t){40000, 100};
    h.block = 50000;
    h.block_words = 16;
    CHECK_EQ(fullcheck_hash_bytes(&h), 40000 + 4 + 100 + 64);
    fullcheck_hash_cursor_t c = {0, 0};
    fullcheck_piece_t p;
    uint32_t expect_off = 0, total = 0;
    unsigned pieces = 0, words = 0;
    while (fullcheck_hash_next(&h, &c, 16384, &p)) {
        pieces++;
        CHECK(p.len <= 16384);
        if (p.size_word) {
            CHECK_EQ(p.size, 0x400);
            words++;
            total += 4;
            continue;
        }
        if (expect_off == 40000 + 100)
            expect_off = 50000;                     // the block's words come last
        CHECK_EQ(p.off, expect_off);
        expect_off += p.len;
        total += p.len;
    }
    CHECK_EQ(words, 1);
    CHECK_EQ(total, fullcheck_hash_bytes(&h));
    CHECK_EQ(pieces, 3 + 1 + 1 + 1);                // 16384, 16384, 7232; the word; 100; 64
    CHECK(!fullcheck_hash_next(&h, &c, 16384, &p));
}

TEST(hash_matches) {
    bootkey_hash_t h = hashed(false);
    CHECK(fullcheck_hash_matches(&h, digest));
    h.value[7] ^= 1;
    CHECK(!fullcheck_hash_matches(&h, digest));
    h.value_words = 7;
    CHECK(fullcheck_hash_matches(&h, digest));
    h.value_words = 0;
    CHECK(!fullcheck_hash_matches(&h, digest));
}

// --- sd-check ------------------------------------------------------------------------------------

static fsck_result_t exfat(void) {
    fsck_result_t res;
    memset(&res, 0, sizeof(res));
    res.kind = FSCK_KIND_EXFAT;
    res.volume_sectors = 15728640;          // 7.5 GB
    res.cluster_bytes = 32768;
    res.passes = res.pass = 2;
    return res;
}

TEST(sd_pass) {
    fsck_result_t res = exfat();
    fullcheck_sd_t in = {FULLCHECK_SD_DONE, &res, true, NULL, 512};
    fullcheck_judge_sd(&in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(is("exFAT encrypted 7.5 GB: no problems, 2 passes in 0.5 s"));
    res.kind = FSCK_KIND_FAT32;
    res.passes = 1;
    res.notes = 3;                          // notes aren't problems
    in.encrypted = false;
    fullcheck_judge_sd(&in, &r);
    CHECK(is("FAT32 7.5 GB: no problems, 1 pass in 0.5 s"));
}

TEST(sd_warn) {
    fsck_result_t res = exfat();
    res.problems = 3;
    res.lost_clusters = 16;
    fullcheck_sd_t in = {FULLCHECK_SD_DONE, &res, true, NULL, 512};
    fullcheck_judge_sd(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("3 problems, 512 KB lost: sd check fix repairs them"));
    res.lost_clusters = 4000000;
    fullcheck_judge_sd(&in, &r);
    CHECK(strlen(r.text) < HEALTH_TEXT_MAX - 1);     // 125000 MB
    res.problems = 1;
    res.lost_clusters = 0;
    fullcheck_judge_sd(&in, &r);
    CHECK(is("1 problem: sd check lists it, sd check fix repairs"));
}

TEST(sd_fail) {
    fsck_result_t res = exfat();
    res.problems = 2;
    res.cross_linked = 4;
    res.lost_clusters = 9;
    fullcheck_sd_t in = {FULLCHECK_SD_DONE, &res, true, NULL, 512};
    fullcheck_judge_sd(&in, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(is("4 cross-linked clusters: sd check fix before writing"));
    res.cross_linked = 0;
    res.reached_free = 5;
    fullcheck_judge_sd(&in, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(is("5 clusters used but free: sd check fix before writing"));
    res.problems = res.reached_free = 4000000000u;
    fullcheck_judge_sd(&in, &r);
    CHECK(strlen(r.text) < HEALTH_TEXT_MAX - 1);
}

TEST(sd_stopped_and_skipped) {
    fsck_result_t res = exfat();
    res.why = "read error at sector 81920";
    fullcheck_sd_t in = {FULLCHECK_SD_FAILED, &res, true, NULL, 0};
    fullcheck_judge_sd(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("check stopped in pass 2: read error at sector 81920"));
    res.passes = 0;
    res.why = "no FAT32 or exFAT volume found";
    fullcheck_judge_sd(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("not checked: no FAT32 or exFAT volume found"));
    res.why = "a reason so long that it would never fit on the line the health table gives it";
    fullcheck_judge_sd(&in, &r);
    CHECK(strlen(r.text) < HEALTH_TEXT_MAX - 1);

    in = (fullcheck_sd_t){FULLCHECK_SD_MOUNTED, NULL, false, NULL, 0};
    fullcheck_judge_sd(&in, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
    CHECK(is("card mounted: sd unmount, then health run sd-check"));
    in.state = FULLCHECK_SD_NOT_BY_HAND;
    fullcheck_judge_sd(&in, &r);
    CHECK(is("not unmounted by hand: sd unmount, then health run sd-check"));
    in.state = FULLCHECK_SD_BUSY;
    in.why = "card being formatted";
    fullcheck_judge_sd(&in, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
    CHECK(is("card being formatted"));
    in.state = FULLCHECK_SD_NOT_OPENED;
    in.why = "encrypted with another board's key";
    fullcheck_judge_sd(&in, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
    CHECK(is("not checked: encrypted with another board's key"));
}

TEST_MAIN(RUN(ram_pass); RUN(ram_scratch_only_and_skip); RUN(ram_warn_and_fail); RUN(image_pass);
          RUN(image_body_changed); RUN(image_item_changed); RUN(image_skip_and_warn);
          RUN(hash_pieces); RUN(hash_matches); RUN(sd_pass); RUN(sd_warn); RUN(sd_fail);
          RUN(sd_stopped_and_skipped);)
