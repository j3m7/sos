// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/pwverify.c: the console password's verifier (PBKDF2-HMAC-SHA256, then
// HMAC-SHA256 under the console-password key), with wolfCrypt built for the host. The expected
// values were computed with Python's hashlib.pbkdf2_hmac and hmac.

#include <stdint.h>
#include <string.h>

#include "core/pwverify.h"
#include "test.h"

static uint8_t salt[PWV_SALT_LEN], key[PWV_LEN];

static void setup(void) {
    for (unsigned i = 0; i < PWV_SALT_LEN; i++)
        salt[i] = (uint8_t)(0x30 + i);
    for (unsigned i = 0; i < PWV_LEN; i++)
        key[i] = (uint8_t)(0x60 + i);
}

static void unhex(const char *hex, uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static bool all_zero(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i])
            return false;
    return true;
}

// The derivation is PBKDF2: the value before the keyed step, for a published-style input.
TEST(pbkdf2_known_answer) {
    pwv_job_t job;
    uint8_t want[32];
    unhex("46f895baaa185f0265c1da708d799953de592f576c4538b7eba99ced700ee75b", want, 32);
    CHECK(pwv_start(&job, "password", 8, (const uint8_t *)"saltSALTsaltSALT", 1000));
    CHECK(pwv_step(&job, 1000));
    CHECK_MEM(job.t, want, 32);
    pwv_abort(&job);
}

TEST(verifier_known_answers) {
    setup();
    uint8_t out[PWV_LEN], want[PWV_LEN];
    unhex("dfa872565c33d94f47768a8712737140f6750f4c9b42dd2ede6d416a5a925833", want, 32);
    CHECK(pwv_compute("correct horse battery", 21, salt, 1000, key, out));
    CHECK_MEM(out, want, 32);
    unhex("aca7783ebb857e070b649d8b9ea1e6f2ac35ed63024a2ee621f6b2bbecd793cf", want, 32);
    CHECK(pwv_compute("x", 1, salt, 2500, key, out));
    CHECK_MEM(out, want, 32);
}

// In slices of any size the result is the same, and a step reports completion only at the end.
TEST(slices_give_the_same_result) {
    setup();
    uint8_t whole[PWV_LEN], sliced[PWV_LEN];
    CHECK(pwv_compute("correct horse battery", 21, salt, 1500, key, whole));
    static const uint32_t sizes[] = { 1, 7, 100, 1498, 1499, 5000 };
    for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        pwv_job_t job;
        CHECK(pwv_start(&job, "correct horse battery", 21, salt, 1500));
        unsigned steps = 0;
        while (!pwv_step(&job, sizes[k]))
            steps++;
        CHECK_EQ(steps, (1499 - 1) / sizes[k]);     // 1499 iterations after the first
        CHECK(pwv_finish(&job, key, sliced));
        CHECK_MEM(sliced, whole, PWV_LEN);
    }
}

TEST(anything_different_gives_another_verifier) {
    setup();
    uint8_t a[PWV_LEN], b[PWV_LEN];
    CHECK(pwv_compute("correct horse battery", 21, salt, 1000, key, a));
    CHECK(pwv_compute("correct horse batterz", 21, salt, 1000, key, b));    // the password
    CHECK(!pwv_equal(a, b));
    CHECK(pwv_compute("correct horse batter", 20, salt, 1000, key, b));     // its length
    CHECK(!pwv_equal(a, b));
    CHECK(pwv_compute("correct horse battery", 21, salt, 1001, key, b));    // the count
    CHECK(!pwv_equal(a, b));
    salt[15] ^= 1;
    CHECK(pwv_compute("correct horse battery", 21, salt, 1000, key, b));    // the salt
    CHECK(!pwv_equal(a, b));
    salt[15] ^= 1;
    key[0] ^= 1;                                                            // another board
    CHECK(pwv_compute("correct horse battery", 21, salt, 1000, key, b));
    CHECK(!pwv_equal(a, b));
    key[0] ^= 1;
    CHECK(pwv_compute("correct horse battery", 21, salt, 1000, key, b));
    CHECK(pwv_equal(a, b));
}

TEST(equal_compares_every_byte) {
    uint8_t a[PWV_LEN], b[PWV_LEN];
    memset(a, 0x5a, sizeof(a));
    memcpy(b, a, sizeof(b));
    CHECK(pwv_equal(a, b));
    for (unsigned i = 0; i < PWV_LEN; i++) {
        b[i] ^= 0x80;
        CHECK(!pwv_equal(a, b));
        b[i] ^= 0x80;
    }
}

TEST(iteration_count_limits) {
    setup();
    pwv_job_t job;
    uint8_t out[PWV_LEN];
    CHECK(!pwv_start(&job, "password", 8, salt, 0));
    CHECK(!pwv_start(&job, "password", 8, salt, PWV_ITERATIONS_MIN - 1));
    CHECK(!pwv_start(&job, "password", 8, salt, PWV_ITERATIONS_MAX + 1));
    memset(out, 0xaa, sizeof(out));
    CHECK(!pwv_compute("password", 8, salt, 1, key, out));      // a stored count of 1 is refused
    CHECK(all_zero(out, sizeof(out)));
    CHECK(pwv_start(&job, "password", 8, salt, PWV_ITERATIONS_MIN));
    pwv_abort(&job);
}

TEST(finish_before_the_end_fails_and_wipes) {
    setup();
    pwv_job_t job;
    uint8_t out[PWV_LEN];
    CHECK(pwv_start(&job, "password", 8, salt, 2000));
    CHECK(!pwv_step(&job, 10));
    memset(out, 0xaa, sizeof(out));
    CHECK(!pwv_finish(&job, key, out));
    CHECK(all_zero(out, sizeof(out)));
    CHECK(all_zero((const uint8_t *)&job, sizeof(job)));        // nothing of the password is left
    // A job that was never started, or already finished, does nothing.
    CHECK(pwv_step(&job, 10));
    CHECK(!pwv_finish(&job, key, out));
}

TEST(finished_job_is_wiped) {
    setup();
    pwv_job_t job;
    uint8_t out[PWV_LEN];
    CHECK(pwv_start(&job, "password", 8, salt, 1000));
    CHECK(pwv_step(&job, 1000));
    CHECK(pwv_finish(&job, key, out));
    CHECK(all_zero((const uint8_t *)&job, sizeof(job)));
}

TEST_MAIN(
    RUN(pbkdf2_known_answer);
    RUN(verifier_known_answers);
    RUN(slices_give_the_same_result);
    RUN(anything_different_gives_another_verifier);
    RUN(equal_compares_every_byte);
    RUN(iteration_count_limits);
    RUN(finish_before_the_end_fails_and_wipes);
    RUN(finished_job_is_wiped);
)
