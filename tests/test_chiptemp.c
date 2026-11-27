// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/chiptemp.c: the chip temperature's conversion, text, lowest and highest
// readings, periodic log and scratch register form (issue #92).

#include <string.h>

#include "core/chiptemp.h"
#include "test.h"

// The datasheet's example (section 12.4.6): 891 at a 3.3 V reference is 20.1 degrees.
TEST(datasheet_example) {
    CHECK_EQ(chiptemp_tenths(891, 1, 3300000), 201);
    CHECK_EQ(chiptemp_tenths(891 * 64, 64, 3300000), 201);
    // The same reading with the reference 1% low: over 4 degrees warmer, as the datasheet says.
    CHECK_EQ(chiptemp_tenths(891, 1, 3267000), 243);
}

TEST(conversion) {
    // 0.706 V is 27.0 degrees: 0.706 / 3.3 * 4096 = 876.3 counts. One count is about 0.47
    // degrees: 876 is 27.14, 877 is 26.67.
    CHECK_EQ(chiptemp_tenths(876, 1, 3300000), 271);
    CHECK_EQ(chiptemp_tenths(877, 1, 3300000), 267);
    // Averaging resolves between counts: 64 samples summing to 876.3 * 64.
    CHECK_EQ(chiptemp_tenths(56083, 64, 3300000), 270);
    // Higher voltage is colder; the ends of the ADC's range stay in 16 bits.
    CHECK(chiptemp_tenths(1000, 1, 3300000) < chiptemp_tenths(900, 1, 3300000));
    CHECK_EQ(chiptemp_tenths(0, 1, 3300000), 4372);
    CHECK_EQ(chiptemp_tenths(4095 * 64, 64, 3300000), -14798);
    CHECK_EQ(chiptemp_tenths(0, 0, 3300000), CHIPTEMP_NONE);
}

// Below zero: rounded to the nearest tenth, not towards zero, and printed with the sign.
TEST(negative) {
    // 935 counts is -0.48 degrees: -0.5, not -0.4.
    CHECK_EQ(chiptemp_tenths(935, 1, 3300000), -5);
    char s[CHIPTEMP_TEXT_MAX];
    chiptemp_format(s, sizeof(s), -5);
    CHECK(!strcmp(s, "-0.5 C"));
    chiptemp_format(s, sizeof(s), -123);
    CHECK(!strcmp(s, "-12.3 C"));
}

TEST(format) {
    char s[CHIPTEMP_TEXT_MAX];
    chiptemp_format(s, sizeof(s), 312);
    CHECK(!strcmp(s, "31.2 C"));
    chiptemp_format(s, sizeof(s), 0);
    CHECK(!strcmp(s, "0.0 C"));
    chiptemp_format(s, sizeof(s), 7);
    CHECK(!strcmp(s, "0.7 C"));
    chiptemp_format(s, sizeof(s), CHIPTEMP_NONE);
    CHECK(!strcmp(s, "no reading"));
    chiptemp_format(s, sizeof(s), -14803);
    CHECK(!strcmp(s, "-1480.3 C"));
    chiptemp_format(s, sizeof(s), INT16_MIN + 1);
    CHECK(!strcmp(s, "-3276.7 C"));
    // Cut to the buffer, always terminated.
    char small[5];
    memset(small, 'x', sizeof(small));
    chiptemp_format(small, 4, 312);
    CHECK(!strcmp(small, "31."));
    CHECK_EQ(small[4], 'x');
}

TEST(format_age) {
    char s[24];
    chiptemp_format_age(s, sizeof(s), 0);
    CHECK(!strcmp(s, "0 s"));
    chiptemp_format_age(s, sizeof(s), 59);
    CHECK(!strcmp(s, "59 s"));
    chiptemp_format_age(s, sizeof(s), 60);
    CHECK(!strcmp(s, "1 min"));
    chiptemp_format_age(s, sizeof(s), 3599);
    CHECK(!strcmp(s, "59 min"));
    chiptemp_format_age(s, sizeof(s), 3600 + 5 * 60 + 30);
    CHECK(!strcmp(s, "1 h 05 min"));
    chiptemp_format_age(s, sizeof(s), 86399);
    CHECK(!strcmp(s, "23 h 59 min"));
    chiptemp_format_age(s, sizeof(s), 2 * 86400 + 4 * 3600 + 59);
    CHECK(!strcmp(s, "2 d 04 h"));
}

TEST(lowest_and_highest) {
    chiptemp_stats_t st = {0};
    chiptemp_stats_add(&st, CHIPTEMP_NONE, 5);     // ignored
    CHECK_EQ(st.count, 0);
    chiptemp_stats_add(&st, 300, 10);
    CHECK_EQ(st.count, 1);
    CHECK_EQ(st.min, 300);
    CHECK_EQ(st.max, 300);
    chiptemp_stats_add(&st, 280, 20);
    chiptemp_stats_add(&st, 320, 30);
    chiptemp_stats_add(&st, 280, 40);              // equal: keeps when it was first reached
    chiptemp_stats_add(&st, 310, 50);
    CHECK_EQ(st.count, 5);
    CHECK_EQ(st.min, 280);
    CHECK_EQ(st.min_s, 20);
    CHECK_EQ(st.max, 320);
    CHECK_EQ(st.max_s, 30);
    CHECK_EQ(st.last, 310);
    CHECK_EQ(st.last_s, 50);
    // Below zero counts as lower.
    chiptemp_stats_add(&st, -15, 60);
    CHECK_EQ(st.min, -15);
}

TEST(log_policy) {
    chiptemp_stats_t st = {0};
    CHECK(!chiptemp_log_due(&st));                 // nothing read yet
    chiptemp_stats_add(&st, 300, 2);
    CHECK(chiptemp_log_due(&st));                  // the first reading after boot
    chiptemp_stats_logged(&st);
    CHECK(!chiptemp_log_due(&st));
    // Steady, or moving less than a degree: not before an hour.
    chiptemp_stats_add(&st, 309, 600);
    CHECK(!chiptemp_log_due(&st));
    chiptemp_stats_add(&st, 291, 1200);
    CHECK(!chiptemp_log_due(&st));
    chiptemp_stats_add(&st, 300, 2 + CHIPTEMP_LOG_MAX_S - 1);
    CHECK(!chiptemp_log_due(&st));
    chiptemp_stats_add(&st, 300, 2 + CHIPTEMP_LOG_MAX_S);
    CHECK(chiptemp_log_due(&st));
    chiptemp_stats_logged(&st);
    uint32_t t0 = st.logged_s;
    // A degree's change, but within a minute of the last line: waits.
    chiptemp_stats_add(&st, 310, t0 + 10);
    CHECK(!chiptemp_log_due(&st));
    chiptemp_stats_add(&st, 310, t0 + CHIPTEMP_LOG_MIN_S);
    CHECK(chiptemp_log_due(&st));
    chiptemp_stats_logged(&st);
    CHECK_EQ(st.logged, 310);
    // Down by a degree from the last line counts too.
    chiptemp_stats_add(&st, 300, st.logged_s + CHIPTEMP_LOG_MIN_S);
    CHECK(chiptemp_log_due(&st));
}

TEST(scratch_register_form) {
    CHECK_EQ(chiptemp_unpack(chiptemp_pack(312)), 312);
    CHECK_EQ(chiptemp_unpack(chiptemp_pack(0)), 0);
    CHECK_EQ(chiptemp_unpack(chiptemp_pack(-5)), -5);
    CHECK_EQ(chiptemp_unpack(chiptemp_pack(-14803)), -14803);
    CHECK_EQ(chiptemp_unpack(chiptemp_pack(INT16_MAX)), INT16_MAX);
    CHECK_EQ(chiptemp_unpack(chiptemp_pack(CHIPTEMP_NONE)), CHIPTEMP_NONE);
    // Out of 16 bits: kept at the end of the range, never wrapped into a plausible reading.
    CHECK_EQ(chiptemp_unpack(chiptemp_pack(40000)), INT16_MAX);
    CHECK_EQ(chiptemp_unpack(chiptemp_pack(-40000)), CHIPTEMP_NONE);
    // What a register holds at power-on, or someone else's marker, is no reading.
    CHECK_EQ(chiptemp_unpack(0), CHIPTEMP_NONE);
    CHECK_EQ(chiptemp_unpack(0x0000012c), CHIPTEMP_NONE);
    CHECK_EQ(chiptemp_unpack(0x474c012c), CHIPTEMP_NONE);
    CHECK_EQ(chiptemp_unpack(0xffffffff), CHIPTEMP_NONE);
}

TEST_MAIN(
    RUN(datasheet_example);
    RUN(conversion);
    RUN(negative);
    RUN(format);
    RUN(format_age);
    RUN(lowest_and_highest);
    RUN(log_policy);
    RUN(scratch_register_form);
)
