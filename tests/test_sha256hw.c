// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/files/sha256hw.c over a fake pico_sha256 made with wolfCrypt
// (tests/fake_sha256.c): the self-check on first use, one holder at a time, the block released
// by finish and by abort, never DMA, and a block that gives a wrong answer never used again.
// The module keeps its self-check's verdict for the process: the broken block is its own run.

#include <stdarg.h>
#include <stdint.h>

#include "fake_sha256.h"
#include "core/log.h"
#include "files/sha256hw.h"
#include "test.h"

static int logged;
static char last_log[200];

void log_msg(int priority, const char *fmt, ...) {
    (void)priority;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    va_end(ap);
    logged++;
}

#ifndef SHA256HW_BROKEN

static const uint8_t abc256[SHA256HW_DIGEST] = {
    0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
    0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
};

static void hash_abc_in_pieces(uint8_t out[SHA256HW_DIGEST]) {
    sha256hw_update((const uint8_t *)"a", 1);
    sha256hw_update((const uint8_t *)"bc", 2);
    sha256hw_finish(out);
}

TEST(a_busy_block_is_refused_and_not_yet_tested) {
    fake_sha256_reset();
    fake_sha256.taken = true;
    CHECK(!sha256hw_begin());
    CHECK_EQ(fake_sha256.starts, 0);
    fake_sha256.taken = false;
    // Refused while busy is not a verdict: the self-check runs once it's free.
    CHECK(sha256hw_begin());
    CHECK_EQ(fake_sha256.starts, 3);        // two vectors, then the hash
    CHECK_EQ(fake_sha256.finished, 2);
    sha256hw_abort();
    CHECK(!fake_sha256.locked);
    CHECK_EQ(logged, 0);
}

TEST(the_self_check_runs_once) {
    fake_sha256_reset();
    CHECK(sha256hw_begin());
    CHECK_EQ(fake_sha256.starts, 1);
    uint8_t d[SHA256HW_DIGEST];
    hash_abc_in_pieces(d);
    CHECK_MEM(d, abc256, sizeof(d));
    CHECK(!fake_sha256.locked);
    CHECK(!fake_sha256.dma);                // CPU-fed: no DMA channel claimed
}

TEST(one_holder_at_a_time_and_its_state_is_kept) {
    fake_sha256_reset();
    CHECK(sha256hw_begin());
    sha256hw_update((const uint8_t *)"a", 1);
    CHECK(!sha256hw_begin());               // a second caller: wolfCrypt
    CHECK(fake_sha256.locked);
    CHECK_EQ(fake_sha256.refused, 0);       // refused before the SDK could clear the state
    sha256hw_update((const uint8_t *)"bc", 2);
    uint8_t d[SHA256HW_DIGEST];
    sha256hw_finish(d);
    CHECK_MEM(d, abc256, sizeof(d));
    CHECK(sha256hw_begin());                // free again
    sha256hw_abort();
}

TEST(abort_releases_and_extra_calls_do_nothing) {
    fake_sha256_reset();
    CHECK(sha256hw_begin());
    sha256hw_update((const uint8_t *)"abc", 3);
    sha256hw_abort();
    CHECK(!fake_sha256.locked);
    CHECK_EQ(fake_sha256.abandoned, 1);
    sha256hw_abort();                       // not held: nothing
    CHECK_EQ(fake_sha256.abandoned, 1);
    sha256hw_update((const uint8_t *)"x", 1);
    CHECK_EQ(fake_sha256.bytes, 3);
    uint8_t d[SHA256HW_DIGEST];
    sha256hw_finish(d);                     // not held: zeros, the fake untouched
    CHECK_EQ(fake_sha256.finished, 0);
    CHECK_EQ(d[0], 0);
    // After an abort the next hash starts clean.
    CHECK(sha256hw_begin());
    hash_abc_in_pieces(d);
    CHECK_MEM(d, abc256, sizeof(d));
}

TEST(someone_else_holding_the_lock_means_wolfcrypt) {
    fake_sha256_reset();
    fake_sha256.taken = true;               // the bootrom, say
    CHECK(!sha256hw_begin());
    CHECK_EQ(fake_sha256.refused, 1);
    fake_sha256.taken = false;
    CHECK(sha256hw_begin());
    sha256hw_abort();
}

TEST_MAIN(
    RUN(a_busy_block_is_refused_and_not_yet_tested);
    RUN(the_self_check_runs_once);
    RUN(one_holder_at_a_time_and_its_state_is_kept);
    RUN(abort_releases_and_extra_calls_do_nothing);
    RUN(someone_else_holding_the_lock_means_wolfcrypt);
)

#else

// The module keeps the self-check's verdict for the process, so a broken block is tested in
// its own executable (test_sha256hw_broken, built with SHA256HW_BROKEN).
TEST(a_wrong_answer_is_logged_once_and_the_block_never_used_again) {
    fake_sha256_reset();
    fake_sha256.wrong = true;
    CHECK(!sha256hw_begin());
    CHECK_EQ(logged, 1);
    CHECK(strstr(last_log, "wrong answer") != NULL);
    CHECK(!fake_sha256.locked);             // the self-check released it
    fake_sha256.wrong = false;              // even if it would answer right now
    int starts = fake_sha256.starts;
    CHECK(!sha256hw_begin());
    CHECK(!sha256hw_begin());
    CHECK_EQ(fake_sha256.starts, starts);   // never started again
    CHECK_EQ(logged, 1);                    // said once
}

TEST_MAIN(
    RUN(a_wrong_answer_is_logged_once_and_the_block_never_used_again);
)

#endif
