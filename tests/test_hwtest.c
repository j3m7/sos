// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/hwtest.c, the pure half of the boot tier's hardware checks: the TRNG's
// startup tests (SP 800-90B §4.4) over made-up streams (good, stuck, a long run, biased, a
// repeating pattern), their cutoffs computed again from the formulas, and the judging of the
// trng, clocks and flash checks.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "core/hwtest.h"
#include "test.h"

// --- streams ---------------------------------------------------------------------------------

static uint64_t rng_state;

// splitmix64: a good, repeatable stand-in for a healthy noise source.
static uint64_t next64(void) {
    uint64_t z = (rng_state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static uint32_t words[HWTEST_TRNG_WORDS];

static void fill_random(uint64_t seed) {
    rng_state = seed;
    for (unsigned i = 0; i < HWTEST_TRNG_WORDS; i++)
        words[i] = (uint32_t)next64();
}

static void set_bit(unsigned i, unsigned v) {
    if (v)
        words[i / 32] |= 1u << (i % 32);
    else
        words[i / 32] &= ~(1u << (i % 32));
}

// Each bit 1 with probability num/den.
static void fill_biased(uint64_t seed, unsigned num, unsigned den) {
    rng_state = seed;
    for (unsigned i = 0; i < HWTEST_TRNG_WORDS * 32; i++)
        set_bit(i, next64() % den < num);
}

// A complete run, judged.
static void judge(health_result_t *r) {
    hwtest_trng_t t;
    memset(&t, 0, sizeof(t));
    t.blocks = HWTEST_TRNG_BLOCKS;
    t.us = 1300;
    hwtest_trng_stats(words, HWTEST_TRNG_WORDS, &t.stats);
    hwtest_judge_trng(&t, r);
}

// --- the cutoffs -----------------------------------------------------------------------------

// SP 800-90B §4.4.1 and §4.4.2 with H = 0.5 and alpha = 2^-30, computed here from the formulas.
TEST(cutoffs_follow_the_formulas) {
    double h = 0.5, alpha = ldexp(1.0, -30);
    CHECK_EQ(1 + (int)ceil(-log2(alpha) / h), HWTEST_TRNG_RCT_CUTOFF);
    // CRITBINOM(W, p, 1 - alpha): the smallest k with P(X <= k) >= 1 - alpha, i.e. with the upper
    // tail P(X > k) <= alpha. Summed from the top, in logs, so no precision is lost.
    int w = HWTEST_TRNG_APT_WINDOW;
    double p = pow(2.0, -h), tail = 0;
    int k = w;
    for (; k >= 0; k--) {
        double pmf = exp(lgamma(w + 1.0) - lgamma(k + 1.0) - lgamma(w - k + 1.0) + k * log(p) +
                         (w - k) * log1p(-p));
        if (tail + pmf > alpha)
            break;                      // P(X > k-1) would be over alpha: k is the smallest
        tail += pmf;
    }
    CHECK_EQ(1 + k, HWTEST_TRNG_APT_CUTOFF);
    CHECK_EQ(HWTEST_TRNG_WORDS * 32, 3 * HWTEST_TRNG_APT_WINDOW);
}

// --- the statistics --------------------------------------------------------------------------

TEST(good_stream_passes) {
    for (uint64_t seed = 1; seed <= 50; seed++) {
        fill_random(seed);
        hwtest_trng_stats_t s;
        hwtest_trng_stats(words, HWTEST_TRNG_WORDS, &s);
        CHECK_EQ(s.bits, 3072);
        CHECK_EQ(s.windows, 3);
        CHECK(s.max_run >= 6 && s.max_run < 25);
        CHECK(s.max_count > 420 && s.max_count < 620);    // the first sample may be the rarer
        health_result_t r;
        judge(&r);
        CHECK_EQ(r.status, HEALTH_PASS);
    }
    fill_random(7);
    health_result_t r;
    judge(&r);
    hwtest_trng_stats_t s;
    hwtest_trng_stats(words, HWTEST_TRNG_WORDS, &s);
    char want[64];
    snprintf(want, sizeof(want), "run %u (fails at 61), proportion %u/1024 (810), 1.3 ms",
             (unsigned)s.max_run, (unsigned)s.max_count);
    CHECK(!strcmp(r.text, want));
}

TEST(stuck_streams_fail) {
    health_result_t r;
    memset(words, 0, sizeof(words));
    judge(&r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(!strcmp(r.text, "repetition count: run of 3072, fails at 61"));
    memset(words, 0xff, sizeof(words));
    hwtest_trng_stats_t s;
    hwtest_trng_stats(words, HWTEST_TRNG_WORDS, &s);
    CHECK_EQ(s.max_run, 3072);
    CHECK_EQ(s.max_count, 1024);
    judge(&r);
    CHECK_EQ(r.status, HEALTH_FAIL);
}

// A run of exactly the cutoff fails; one shorter passes. Across a word boundary too.
TEST(repetition_count_at_its_cutoff) {
    health_result_t r;
    for (unsigned start = 100; start <= 1000; start += 900) {
        for (unsigned len = HWTEST_TRNG_RCT_CUTOFF - 1; len <= HWTEST_TRNG_RCT_CUTOFF; len++) {
            fill_random(3);
            // The bits on either side differ from the run, so it is exactly len long.
            set_bit(start - 1, 0);
            for (unsigned i = 0; i < len; i++)
                set_bit(start + i, 1);
            set_bit(start + len, 0);
            hwtest_trng_stats_t s;
            hwtest_trng_stats(words, HWTEST_TRNG_WORDS, &s);
            CHECK_EQ(s.max_run, len);
            judge(&r);
            CHECK_EQ(r.status, len >= HWTEST_TRNG_RCT_CUTOFF ? HEALTH_FAIL : HEALTH_PASS);
        }
    }
    CHECK(!strcmp(r.text, "repetition count: run of 61, fails at 61"));
}

// The second window holds `ones` ones, its first sample a one, the zeros spread out so no run is
// long.
static void window_with(unsigned ones) {
    fill_random(11);
    unsigned zeros = HWTEST_TRNG_APT_WINDOW - ones, base = HWTEST_TRNG_APT_WINDOW;
    for (unsigned j = 0; j < HWTEST_TRNG_APT_WINDOW; j++)
        set_bit(base + j, 1);
    for (unsigned z = 0; z < zeros; z++)
        set_bit(base + 1 + z * (HWTEST_TRNG_APT_WINDOW - 1) / zeros, 0);
}

TEST(adaptive_proportion_at_its_cutoff) {
    health_result_t r;
    hwtest_trng_stats_t s;
    window_with(HWTEST_TRNG_APT_CUTOFF - 1);
    hwtest_trng_stats(words, HWTEST_TRNG_WORDS, &s);
    CHECK_EQ(s.max_count, HWTEST_TRNG_APT_CUTOFF - 1);
    CHECK(s.max_run < HWTEST_TRNG_RCT_CUTOFF);
    judge(&r);
    CHECK_EQ(r.status, HEALTH_PASS);
    window_with(HWTEST_TRNG_APT_CUTOFF);
    hwtest_trng_stats(words, HWTEST_TRNG_WORDS, &s);
    CHECK_EQ(s.max_count, HWTEST_TRNG_APT_CUTOFF);
    judge(&r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(!strcmp(r.text, "adaptive proportion: 810 of 1024, fails at 810"));
}

TEST(biased_streams) {
    health_result_t r;
    fill_biased(5, 85, 100);            // 85 % ones: about 870 a window
    judge(&r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    fill_biased(5, 60, 100);            // 60 %: within what H = 0.5 allows
    judge(&r);
    CHECK_EQ(r.status, HEALTH_PASS);
}

// A short repeating pattern has no long run and no bias: these two tests don't see it. The
// TRNG's own CRNGT does (two equal 16-bit blocks in a row), and its errors fail the check.
TEST(repeating_pattern_is_left_to_the_hardware) {
    health_result_t r;
    memset(words, 0xaa, sizeof(words));
    judge(&r);
    CHECK_EQ(r.status, HEALTH_PASS);
    hwtest_trng_t t;
    memset(&t, 0, sizeof(t));
    t.crngt_errors = HWTEST_TRNG_CRNGT_MAX + 1;
    t.us = 400;
    hwtest_judge_trng(&t, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(!strcmp(r.text, "TRNG threw away 9 blocks (crngt 9, vn 0), 0 of 16 kept"));
}

TEST(hardware_errors_and_short_runs) {
    health_result_t r;
    hwtest_trng_t t;
    fill_random(9);
    memset(&t, 0, sizeof(t));
    t.blocks = HWTEST_TRNG_BLOCKS;
    t.crngt_errors = HWTEST_TRNG_CRNGT_MAX;     // a few thrown away: still fine
    hwtest_trng_stats(words, HWTEST_TRNG_WORDS, &t.stats);
    hwtest_judge_trng(&t, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    t.crngt_errors = HWTEST_TRNG_CRNGT_MAX / 2;
    t.vn_errors = HWTEST_TRNG_CRNGT_MAX / 2;   // the two kinds count together
    hwtest_judge_trng(&t, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    t.vn_errors++;
    hwtest_judge_trng(&t, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(!strcmp(r.text, "TRNG threw away 9 blocks (crngt 4, vn 5), 16 of 16 kept"));
    t.vn_errors = 0;
    t.blocks = 3;                       // stopped by the block's own test: says when
    t.autocorr_error = true;
    hwtest_judge_trng(&t, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(!strcmp(r.text, "TRNG autocorrelation error after 3 of 16 blocks"));
    memset(&t, 0, sizeof(t));
    t.blocks = 3;
    t.us = 10000;
    hwtest_trng_stats(words, 3 * HWTEST_TRNG_BLOCK_WORDS, &t.stats);
    hwtest_judge_trng(&t, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(!strcmp(r.text, "the TRNG gave 3 of 16 blocks in 10 ms"));
}

// --- clocks ----------------------------------------------------------------------------------

static hwtest_clock_t clocks[5];

static void clocks_as_set(void) {
    static const hwtest_clock_t set[5] = {
        {"clk_sys", 150000000, 150000}, {"clk_peri", 150000000, 150000},
        {"clk_usb", 48000000, 48000},   {"clk_adc", 48000000, 48000},
        {"clk_hstx", 0, 0},             // not running: skipped
    };
    memcpy(clocks, set, sizeof(clocks));
}

TEST(clocks_judging) {
    health_result_t r;
    clocks_as_set();
    hwtest_judge_clocks(clocks, 5, 32, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(!strcmp(r.text, "clk_sys 150.0 MHz; 4 clocks within 0.00%; lposc 32 kHz"));
    clocks[2].got_khz = 48001;          // FC0's resolution: 1 kHz
    hwtest_judge_clocks(clocks, 5, 32, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(!strcmp(r.text, "clk_sys 150.0 MHz; 4 clocks within 0.01%; lposc 32 kHz"));
    // 1 % is still a pass; past it a warning; past 5 % a failure.
    clocks[2].got_khz = 48480;
    hwtest_judge_clocks(clocks, 5, 32, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    clocks[2].got_khz = 47200;
    hwtest_judge_clocks(clocks, 5, 32, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(!strcmp(r.text, "clk_usb 47.2 MHz, set to 48.0 (-1.67%)"));
    clocks[1].got_khz = 160000;         // the worst is named
    hwtest_judge_clocks(clocks, 5, 32, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(!strcmp(r.text, "clk_peri 160.0 MHz, set to 150.0 (+6.67%)"));
    clocks[1].got_khz = 0;              // stopped
    hwtest_judge_clocks(clocks, 5, 32, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(!strcmp(r.text, "clk_peri 0.0 MHz, set to 150.0 (-100.00%)"));
}

// The LPOSC's band catches a crystal about 2x off (everything counted against clk_ref is then
// off by the same factor, so only the LPOSC shows it).
TEST(lposc_cross_check) {
    health_result_t r;
    clocks_as_set();
    hwtest_judge_clocks(clocks, 5, 18, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    hwtest_judge_clocks(clocks, 5, 54, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    hwtest_judge_clocks(clocks, 5, 16, &r);     // a 24 MHz crystal taken for 12
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(!strcmp(r.text, "lposc 16 kHz, not 18-54: crystal wrong?"));
    hwtest_judge_clocks(clocks, 5, 65, &r);     // a 6 MHz one
    CHECK_EQ(r.status, HEALTH_WARN);
    hwtest_judge_clocks(clocks, 5, 0, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    clocks[0].got_khz = 140000;         // a clock's own failure comes first
    hwtest_judge_clocks(clocks, 5, 0, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    hwtest_clock_t none = {"clk_hstx", 0, 0};
    hwtest_judge_clocks(&none, 1, 32, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
}

// --- flash -----------------------------------------------------------------------------------

static health_status_t flash(uint8_t a, uint8_t b, uint8_t c, health_result_t *r) {
    uint8_t id[3] = {a, b, c};
    hwtest_judge_flash(id, 4u * 1024 * 1024, r);
    return r->status;
}

TEST(flash_judging) {
    health_result_t r;
    CHECK_EQ(flash(0xef, 0x40, 0x16, &r), HEALTH_PASS);
    CHECK(!strcmp(r.text, "JEDEC ef 40 16 (Winbond, 4 MB)"));
    CHECK_EQ(flash(0xc8, 0x40, 0x18, &r), HEALTH_PASS);         // bigger is fine
    CHECK(!strcmp(r.text, "JEDEC c8 40 18 (GigaDevice, 16 MB)"));
    CHECK_EQ(flash(0xef, 0x40, 0x15, &r), HEALTH_FAIL);
    CHECK(!strcmp(r.text, "JEDEC ef 40 15 (Winbond, 2 MB): the build needs 4 MB"));
    CHECK_EQ(flash(0xef, 0x40, 0x10, &r), HEALTH_FAIL);
    CHECK(!strcmp(r.text, "JEDEC ef 40 10 (Winbond, 64 KB): the build needs 4 MB"));
    CHECK_EQ(flash(0x12, 0x40, 0x16, &r), HEALTH_WARN);
    CHECK(!strcmp(r.text, "JEDEC 12 40 16 (unknown maker, 4 MB)"));
    CHECK_EQ(flash(0x12, 0x40, 0x15, &r), HEALTH_FAIL);         // too small wins
    CHECK_EQ(flash(0xef, 0x40, 0x41, &r), HEALTH_WARN);
    CHECK(!strcmp(r.text, "JEDEC ef 40 41 (Winbond, size code unknown)"));
    CHECK_EQ(flash(0xff, 0xff, 0xff, &r), HEALTH_WARN);
    CHECK(!strcmp(r.text, "no JEDEC ID (ff ff ff)"));
    CHECK_EQ(flash(0, 0, 0, &r), HEALTH_WARN);
}

TEST_MAIN(
    RUN(cutoffs_follow_the_formulas);
    RUN(good_stream_passes);
    RUN(stuck_streams_fail);
    RUN(repetition_count_at_its_cutoff);
    RUN(adaptive_proportion_at_its_cutoff);
    RUN(biased_streams);
    RUN(repeating_pattern_is_left_to_the_hardware);
    RUN(hardware_errors_and_short_runs);
    RUN(clocks_judging);
    RUN(lposc_cross_check);
    RUN(flash_judging);
)
