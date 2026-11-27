// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/files/sdfault.c: `sd fault`'s write count and when a fault fires.

#include <stdint.h>

#include "files/sdfault.h"
#include "test.h"

// Writes `n` times; returns how many of them were told to pass.
static unsigned passes(unsigned n) {
    unsigned p = 0;
    for (unsigned i = 0; i < n; i++)
        p += sdfault_write() == SDFAULT_PASS;
    return p;
}

TEST(off_by_default_and_not_counting) {
    sdfault_off();
    CHECK_EQ(passes(1000), 1000);
    CHECK(!sdfault_state()->counting);
    CHECK_EQ(sdfault_state()->writes, 0);
    CHECK_EQ(sdfault_state()->armed, SDFAULT_OFF);
}

TEST(fail_fires_once_at_n) {
    CHECK(sdfault_arm(SDFAULT_FAIL, 5));
    CHECK_EQ(sdfault_state()->armed, SDFAULT_FAIL);
    CHECK_EQ(sdfault_state()->at, 5);
    CHECK_EQ(passes(4), 4);
    CHECK_EQ(sdfault_state()->writes, 4);
    CHECK_EQ(sdfault_write(), SDFAULT_DO_FAIL);
    CHECK_EQ(sdfault_state()->armed, SDFAULT_OFF);
    CHECK_EQ(sdfault_state()->fired, SDFAULT_FAIL);
    // Later writes go through, and are still counted.
    CHECK_EQ(passes(100), 100);
    CHECK_EQ(sdfault_state()->writes, 105);
    CHECK(sdfault_state()->counting);
}

TEST(first_write_is_one) {
    CHECK(sdfault_arm(SDFAULT_REBOOT, 1));
    CHECK_EQ(sdfault_write(), SDFAULT_DO_REBOOT);
    CHECK_EQ(sdfault_state()->writes, 1);
    CHECK_EQ(sdfault_state()->fired, SDFAULT_REBOOT);
}

TEST(rearm_replaces_and_restarts_count) {
    CHECK(sdfault_arm(SDFAULT_FAIL, 3));
    CHECK_EQ(passes(2), 2);
    CHECK(sdfault_arm(SDFAULT_REBOOT, 3));
    CHECK_EQ(sdfault_state()->writes, 0);
    CHECK_EQ(sdfault_state()->fired, SDFAULT_OFF);
    CHECK_EQ(passes(2), 2);
    CHECK_EQ(sdfault_write(), SDFAULT_DO_REBOOT);
}

TEST(off_disarms_and_stops_counting) {
    CHECK(sdfault_arm(SDFAULT_FAIL, 2));
    CHECK_EQ(passes(1), 1);
    sdfault_off();
    CHECK_EQ(passes(10), 10);
    CHECK(!sdfault_state()->counting);
    CHECK_EQ(sdfault_state()->writes, 0);
}

TEST(bad_arguments_change_nothing) {
    CHECK(sdfault_arm(SDFAULT_FAIL, 7));
    CHECK_EQ(passes(3), 3);
    CHECK(!sdfault_arm(SDFAULT_FAIL, 0));
    CHECK(!sdfault_arm(SDFAULT_REBOOT, SDFAULT_MAX_N + 1));
    CHECK(!sdfault_arm(SDFAULT_OFF, 4));
    CHECK_EQ(sdfault_state()->armed, SDFAULT_FAIL);
    CHECK_EQ(sdfault_state()->at, 7);
    CHECK_EQ(sdfault_state()->writes, 3);
    CHECK(sdfault_arm(SDFAULT_REBOOT, SDFAULT_MAX_N));
    sdfault_off();
}

TEST(large_n_fires) {
    CHECK(sdfault_arm(SDFAULT_FAIL, 70000));
    CHECK_EQ(passes(69999), 69999);
    CHECK_EQ(sdfault_write(), SDFAULT_DO_FAIL);
    sdfault_off();
}

TEST_MAIN(
    RUN(off_by_default_and_not_counting);
    RUN(fail_fires_once_at_n);
    RUN(first_write_is_one);
    RUN(rearm_replaces_and_restarts_count);
    RUN(off_disarms_and_stops_counting);
    RUN(bad_arguments_change_nothing);
    RUN(large_n_fires);
)
