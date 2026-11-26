// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/failcount.c: the count of wrong console passwords kept in flash, on
// a fake flash sector that behaves like the real one (programming can only clear bits; an
// erase sets them all).

#include <string.h>

#include "core/failcount.h"
#include "test.h"

static uint8_t flash[FAILCOUNT_SECTOR];
static unsigned programs, erases;
static int fail_program_at = -1;        // the nth program from now fails (0 = the next)
static bool fail_erase;
static bool cut_power;                  // a program is "cut": only half of its bits land
static bool silent_program;             // a program reports success and changes nothing
static bool silent_erase;               // an erase reports success and changes nothing

static const uint8_t *f_read(void *ctx) { (void)ctx; return flash; }
static bool f_program(void *ctx, unsigned page, const uint8_t *data) {
    (void)ctx;
    CHECK(page < FAILCOUNT_SECTOR / FAILCOUNT_PAGE);
    if (fail_program_at == 0) {
        fail_program_at = -1;
        return false;
    }
    if (fail_program_at > 0)
        fail_program_at--;
    programs++;
    if (silent_program)
        return true;
    for (unsigned i = 0; i < FAILCOUNT_PAGE; i++) {
        if (cut_power && i >= FAILCOUNT_PAGE / 2)
            break;
        flash[page * FAILCOUNT_PAGE + i] &= data[i];    // bits only go from 1 to 0
    }
    return true;
}
static bool f_erase(void *ctx) {
    (void)ctx;
    if (fail_erase)
        return false;
    erases++;
    if (!silent_erase)
        memset(flash, 0xff, sizeof(flash));
    return true;
}
static const failcount_store_t store = { NULL, f_read, f_program, f_erase };

static void fresh(void) {
    memset(flash, 0xff, sizeof(flash));
    programs = erases = 0;
    fail_program_at = -1;
    fail_erase = cut_power = silent_program = silent_erase = false;
    failcount_init(&store);
}
static void reboot(void) { failcount_init(&store); }

TEST(blank_sector_is_zero) {
    fresh();
    CHECK_EQ(failcount_get(), 0);
    CHECK_EQ(programs, 0);              // reading writes nothing
    CHECK_EQ(erases, 0);
    CHECK(failcount_clear());           // nothing to clear, nothing written
    CHECK_EQ(programs, 0);
}

TEST(counts_and_survives_a_reboot) {
    fresh();
    for (unsigned i = 1; i <= 5; i++) {
        CHECK(failcount_attempt());
        CHECK_EQ(failcount_get(), i);
        reboot();
        CHECK_EQ(failcount_get(), i);
    }
}

// The point of it: the attempt is in flash BEFORE the check runs. A reset during the check
// (never reaching "wrong" or "right") leaves it counted.
TEST(an_attempt_cut_short_by_a_reset_is_counted) {
    fresh();
    CHECK(failcount_attempt());
    reboot();                           // the check never finished
    CHECK_EQ(failcount_get(), 1);
    CHECK(failcount_attempt());
    reboot();
    CHECK_EQ(failcount_get(), 2);
}

TEST(a_right_password_clears_it) {
    fresh();
    for (int i = 0; i < 3; i++)
        CHECK(failcount_attempt());
    CHECK(failcount_attempt());         // the one that turns out right
    CHECK(failcount_clear());
    CHECK_EQ(failcount_get(), 0);
    reboot();
    CHECK_EQ(failcount_get(), 0);
    CHECK(failcount_attempt());         // and it counts from zero again
    CHECK_EQ(failcount_get(), 1);
    reboot();
    CHECK_EQ(failcount_get(), 1);
}

// Counting and clearing need no erase: the first attempt on a blank sector erases once (the
// sector may have held anything) and writes the marker; after that only bits are cleared.
TEST(no_erase_in_normal_use) {
    fresh();
    CHECK(failcount_attempt());
    CHECK_EQ(erases, 1);
    for (int i = 0; i < 200; i++) {
        CHECK(failcount_attempt());
        if (i % 3 == 0)
            CHECK(failcount_clear());
    }
    CHECK_EQ(erases, 1);
}

// Whatever the sector held before isn't a count.
TEST(garbage_is_zero_and_is_replaced) {
    fresh();
    for (size_t i = 0; i < sizeof(flash); i++)
        flash[i] = (uint8_t)(i * 7);
    reboot();
    CHECK_EQ(failcount_get(), 0);
    CHECK(failcount_attempt());
    CHECK_EQ(erases, 1);
    CHECK_EQ(failcount_get(), 1);
    reboot();
    CHECK_EQ(failcount_get(), 1);
    memset(flash, 0, sizeof(flash));    // all zero: no marker either
    reboot();
    CHECK_EQ(failcount_get(), 0);
}

// Many wrong answers between right ones: clearing spans pages of the track.
TEST(clearing_across_pages) {
    fresh();
    unsigned n = FAILCOUNT_PAGE * 8 * 2 + 37;           // more than two pages of bits
    for (unsigned i = 0; i < n; i++)
        CHECK(failcount_attempt());
    CHECK_EQ(failcount_get(), n);
    reboot();
    CHECK_EQ(failcount_get(), n);
    CHECK(failcount_clear());
    CHECK_EQ(failcount_get(), 0);
    reboot();
    CHECK_EQ(failcount_get(), 0);
    CHECK(failcount_attempt());
    reboot();
    CHECK_EQ(failcount_get(), 1);
}

// The attempts track fills up: the sector is erased and what is outstanding is carried over.
TEST(full_track_starts_over_keeping_the_count) {
    fresh();
    for (unsigned i = 0; i < FAILCOUNT_CAPACITY - 3; i++)
        CHECK(failcount_attempt());
    CHECK(failcount_clear());
    for (int i = 0; i < 3; i++)
        CHECK(failcount_attempt());     // the track is now full, 3 outstanding
    CHECK_EQ(failcount_get(), 3);
    CHECK_EQ(erases, 1);
    CHECK(failcount_attempt());         // no room: erase, carry 3, count this one
    CHECK_EQ(erases, 2);
    CHECK_EQ(failcount_get(), 4);
    reboot();
    CHECK_EQ(failcount_get(), 4);
    CHECK(failcount_clear());
    reboot();
    CHECK_EQ(failcount_get(), 0);
}

// Full of nothing but wrong answers: the carried count is capped, still far above what the
// longest delay needs.
TEST(full_track_of_failures_is_capped) {
    fresh();
    for (unsigned i = 0; i < FAILCOUNT_CAPACITY; i++)
        CHECK(failcount_attempt());
    CHECK_EQ(failcount_get(), FAILCOUNT_CAPACITY);
    CHECK(failcount_attempt());
    CHECK_EQ(failcount_get(), FAILCOUNT_CARRY_MAX + 1);
    reboot();
    CHECK_EQ(failcount_get(), FAILCOUNT_CARRY_MAX + 1);
}

// Flash that can't be written: the count still goes up in RAM, so the delays grow until the
// next reboot, and the caller is told.
TEST(write_failures_count_in_ram) {
    fresh();
    CHECK(failcount_attempt());
    fail_program_at = 0;
    CHECK(!failcount_attempt());
    CHECK_EQ(failcount_get(), 2);
    fail_program_at = 0;
    CHECK(!failcount_attempt());
    CHECK_EQ(failcount_get(), 3);
    reboot();
    CHECK_EQ(failcount_get(), 1);       // only the written one survived
    fresh();
    fail_erase = true;
    CHECK(!failcount_attempt());        // a blank sector that can't be started
    CHECK_EQ(failcount_get(), 1);
    fail_erase = false;
    CHECK(failcount_attempt());
    CHECK_EQ(failcount_get(), 2);       // one in RAM, one in flash
    CHECK(failcount_clear());
    CHECK_EQ(failcount_get(), 0);
}

// A clear that fails partway is not reported as done, and what a reboot will see is what the
// count says.
TEST(a_failed_clear_is_not_a_clear) {
    fresh();
    unsigned n = FAILCOUNT_PAGE * 8 + 5;                // the clear needs two programs
    for (unsigned i = 0; i < n; i++)
        CHECK(failcount_attempt());
    fail_program_at = 1;                                // the second one fails
    CHECK(!failcount_clear());
    unsigned left = failcount_get();
    CHECK(left > 0 && left < n);
    reboot();
    CHECK_EQ(failcount_get(), left);
    CHECK(failcount_clear());
    CHECK_EQ(failcount_get(), 0);
}

// Power lost while the attempt's bit is being written: it lands or it doesn't, and either way
// the next boot reads a sane count and carries on.
TEST(a_cut_write_leaves_a_sane_count) {
    fresh();
    CHECK(failcount_attempt());
    CHECK(failcount_attempt());
    cut_power = true;
    failcount_attempt();
    cut_power = false;
    reboot();
    unsigned c = failcount_get();
    CHECK(c == 2 || c == 3);
    CHECK(failcount_attempt());
    CHECK_EQ(failcount_get(), c + 1);
    CHECK(failcount_clear());
    reboot();
    CHECK_EQ(failcount_get(), 0);
}

// The flash call itself reports nothing on the board, so a write that didn't take must be
// found by reading back: it is reported, and the count still goes up in RAM.
TEST(a_write_that_did_not_take_is_noticed) {
    fresh();
    CHECK(failcount_attempt());
    silent_program = true;
    CHECK(!failcount_attempt());
    CHECK_EQ(failcount_get(), 2);
    CHECK(!failcount_attempt());
    CHECK_EQ(failcount_get(), 3);
    silent_program = false;
    CHECK(failcount_attempt());
    CHECK_EQ(failcount_get(), 4);
    reboot();
    CHECK_EQ(failcount_get(), 2);       // the two that did land
    // A clear that didn't take isn't a clear.
    silent_program = true;
    CHECK(!failcount_clear());
    silent_program = false;
    reboot();
    CHECK_EQ(failcount_get(), 2);
    CHECK(failcount_clear());
    reboot();
    CHECK_EQ(failcount_get(), 0);
    // An erase that didn't take, over garbage: nothing is trusted, the count stays in RAM.
    fresh();
    memset(flash, 0x5a, sizeof(flash));
    reboot();
    silent_erase = true;
    CHECK(!failcount_attempt());
    CHECK_EQ(failcount_get(), 1);
    CHECK(!failcount_attempt());
    CHECK_EQ(failcount_get(), 2);
    silent_erase = false;
    CHECK(failcount_attempt());
    CHECK_EQ(failcount_get(), 3);
}

// The attempts track is full and starting over fails after the erase: the outstanding count
// was only in the erased track. It must not drop: it is carried in RAM.
TEST(a_failed_restart_keeps_the_count) {
    fresh();
    for (unsigned i = 0; i < FAILCOUNT_CAPACITY - 5; i++)
        CHECK(failcount_attempt());
    CHECK(failcount_clear());
    for (int i = 0; i < 5; i++)
        CHECK(failcount_attempt());     // full, 5 outstanding
    CHECK_EQ(failcount_get(), 5);
    fail_program_at = 0;                // the marker's write, right after the erase
    CHECK(!failcount_attempt());
    CHECK_EQ(failcount_get(), 6);       // not 1
    CHECK(failcount_attempt());         // flash works again: counted there from now on
    CHECK_EQ(failcount_get(), 7);
    fresh();
    for (unsigned i = 0; i < FAILCOUNT_CAPACITY - 5; i++)
        CHECK(failcount_attempt());
    CHECK(failcount_clear());
    for (int i = 0; i < 5; i++)
        CHECK(failcount_attempt());
    fail_program_at = 1;                // the marker lands, the carry's write fails
    CHECK(!failcount_attempt());
    CHECK_EQ(failcount_get(), 6);
    fail_erase = true;                  // or the erase itself fails: the full track stays
    fresh();
    fail_erase = false;
    for (unsigned i = 0; i < FAILCOUNT_CAPACITY; i++)
        CHECK(failcount_attempt());
    fail_erase = true;
    CHECK(!failcount_attempt());
    CHECK_EQ(failcount_get(), FAILCOUNT_CAPACITY + 1);
    reboot();
    CHECK_EQ(failcount_get(), FAILCOUNT_CAPACITY);
}

// Only this sector's own bits are touched, and never set: every program is one page, and the
// marker stays.
TEST(marker_stays) {
    fresh();
    for (int i = 0; i < 20; i++)
        CHECK(failcount_attempt());
    CHECK(failcount_clear());
    CHECK(!memcmp(flash, "SOSFAIL1", 8));
    for (unsigned i = 8; i < FAILCOUNT_PAGE; i++)
        CHECK_EQ(flash[i], 0xff);       // the rest of the marker's page is untouched
}

TEST_MAIN(
    RUN(blank_sector_is_zero);
    RUN(counts_and_survives_a_reboot);
    RUN(an_attempt_cut_short_by_a_reset_is_counted);
    RUN(a_right_password_clears_it);
    RUN(no_erase_in_normal_use);
    RUN(garbage_is_zero_and_is_replaced);
    RUN(clearing_across_pages);
    RUN(full_track_starts_over_keeping_the_count);
    RUN(full_track_of_failures_is_capped);
    RUN(write_failures_count_in_ram);
    RUN(a_failed_clear_is_not_a_clear);
    RUN(a_cut_write_leaves_a_sane_count);
    RUN(a_write_that_did_not_take_is_noticed);
    RUN(a_failed_restart_keeps_the_count);
    RUN(marker_stays);
)
