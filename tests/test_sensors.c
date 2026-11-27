// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/sensors.c: chip temperature and supply voltage (issue #92): the
// conversions, text, lowest and highest readings, periodic log and scratch register form.

#include <string.h>

#include "core/sensors.h"
#include "test.h"

// --- temperature ---------------------------------------------------------------------------------

// The datasheet's example (section 12.4.6): 891 at a 3.3 V reference is 20.1 degrees.
TEST(temp_datasheet_example) {
    CHECK_EQ(sensors_temp_tenths(891, 1, 3300000), 201);
    CHECK_EQ(sensors_temp_tenths(891 * 64, 64, 3300000), 201);
    // The same reading with the reference 1% low: over 4 degrees warmer, as the datasheet says.
    CHECK_EQ(sensors_temp_tenths(891, 1, 3267000), 243);
}

TEST(temp_conversion) {
    // 0.706 V is 27.0 degrees: 0.706 / 3.3 * 4096 = 876.3 counts. One count is about 0.47
    // degrees: 876 is 27.14, 877 is 26.67.
    CHECK_EQ(sensors_temp_tenths(876, 1, 3300000), 271);
    CHECK_EQ(sensors_temp_tenths(877, 1, 3300000), 267);
    // Averaging resolves between counts: 64 samples summing to 876.3 * 64.
    CHECK_EQ(sensors_temp_tenths(56083, 64, 3300000), 270);
    // Higher voltage is colder; the ends of the ADC's range stay in 16 bits.
    CHECK(sensors_temp_tenths(1000, 1, 3300000) < sensors_temp_tenths(900, 1, 3300000));
    CHECK_EQ(sensors_temp_tenths(0, 1, 3300000), 4372);
    CHECK_EQ(sensors_temp_tenths(4095 * 64, 64, 3300000), -14798);
    CHECK_EQ(sensors_temp_tenths(0, 0, 3300000), SENSORS_NONE);
}

// Below zero: rounded to the nearest tenth, not towards zero, and printed with the sign.
TEST(temp_negative) {
    // 935 counts is -0.48 degrees: -0.5, not -0.4.
    CHECK_EQ(sensors_temp_tenths(935, 1, 3300000), -5);
    char s[SENSORS_TEXT_MAX];
    sensors_format_temp(s, sizeof(s), -5);
    CHECK(!strcmp(s, "-0.5 C"));
    sensors_format_temp(s, sizeof(s), -123);
    CHECK(!strcmp(s, "-12.3 C"));
}

TEST(temp_format) {
    char s[SENSORS_TEXT_MAX];
    sensors_format_temp(s, sizeof(s), 312);
    CHECK(!strcmp(s, "31.2 C"));
    sensors_format_temp(s, sizeof(s), 0);
    CHECK(!strcmp(s, "0.0 C"));
    sensors_format_temp(s, sizeof(s), 7);
    CHECK(!strcmp(s, "0.7 C"));
    sensors_format_temp(s, sizeof(s), SENSORS_NONE);
    CHECK(!strcmp(s, "no reading"));
    sensors_format_temp(s, sizeof(s), -14803);
    CHECK(!strcmp(s, "-1480.3 C"));
    sensors_format_temp(s, sizeof(s), INT16_MIN + 1);
    CHECK(!strcmp(s, "-3276.7 C"));
    // Cut to the buffer, always terminated.
    char small[5];
    memset(small, 'x', sizeof(small));
    sensors_format_temp(small, 4, 312);
    CHECK(!strcmp(small, "31."));
    CHECK_EQ(small[4], 'x');
}

// --- supply voltage ------------------------------------------------------------------------------

TEST(vsys_conversion) {
    // dev-1 on USB: 1885 counts settled, a third of VSYS: 1885 * 3.3 / 4096 * 3 = 4.556 V.
    CHECK_EQ(sensors_vsys_mv(1885, 1, 3300000, 3), 4556);
    CHECK_EQ(sensors_vsys_mv(1885 * 64, 64, 3300000, 3), 4556);
    // Without the divider, the pin's own voltage: 1241 counts is 0.9998 V.
    CHECK_EQ(sensors_vsys_mv(1241, 1, 3300000, 1), 1000);
    // The ends of the range: 0, and full scale (9.9 V, still in 16 bits).
    CHECK_EQ(sensors_vsys_mv(0, 1, 3300000, 3), 0);
    CHECK_EQ(sensors_vsys_mv(4095 * 64, 64, 3300000, 3), 9898);
    // One count is 2.4 mV of VSYS, rounded to the nearest millivolt.
    CHECK_EQ(sensors_vsys_mv(1, 1, 3300000, 3), 2);
    CHECK_EQ(sensors_vsys_mv(0, 0, 3300000, 3), SENSORS_NONE);
    // The reference scales it: 1% low reads 1% low.
    CHECK_EQ(sensors_vsys_mv(1885, 1, 3267000, 3), 4510);
}

TEST(vsys_format) {
    char s[SENSORS_TEXT_MAX];
    sensors_format_mv(s, sizeof(s), 4556);
    CHECK(!strcmp(s, "4.56 V"));
    sensors_format_mv(s, sizeof(s), 4995);         // to the nearest 10 mV, halves up
    CHECK(!strcmp(s, "5.00 V"));
    sensors_format_mv(s, sizeof(s), 4994);
    CHECK(!strcmp(s, "4.99 V"));
    sensors_format_mv(s, sizeof(s), 1800);
    CHECK(!strcmp(s, "1.80 V"));
    sensors_format_mv(s, sizeof(s), 9);
    CHECK(!strcmp(s, "0.01 V"));
    sensors_format_mv(s, sizeof(s), 0);
    CHECK(!strcmp(s, "0.00 V"));
    sensors_format_mv(s, sizeof(s), 9898);
    CHECK(!strcmp(s, "9.90 V"));
    sensors_format_mv(s, sizeof(s), SENSORS_NONE);
    CHECK(!strcmp(s, "no reading"));
}

TEST(format_both) {
    char s[SENSORS_BOTH_MAX];
    sensors_format_both(s, sizeof(s), 267, 4556);
    CHECK(!strcmp(s, "chip 26.7 C, supply 4.56 V"));
    sensors_format_both(s, sizeof(s), 267, SENSORS_NONE);     // VSYS not read yet
    CHECK(!strcmp(s, "chip 26.7 C"));
    sensors_format_both(s, sizeof(s), SENSORS_NONE, SENSORS_NONE);
    CHECK(!strcmp(s, "no readings"));
    // The longest: still fits.
    sensors_format_both(s, sizeof(s), -14803, 9898);
    CHECK(!strcmp(s, "chip -1480.3 C, supply 9.90 V"));
}

// --- shared --------------------------------------------------------------------------------------

TEST(format_age) {
    char s[24];
    sensors_format_age(s, sizeof(s), 0);
    CHECK(!strcmp(s, "0 s"));
    sensors_format_age(s, sizeof(s), 59);
    CHECK(!strcmp(s, "59 s"));
    sensors_format_age(s, sizeof(s), 60);
    CHECK(!strcmp(s, "1 min"));
    sensors_format_age(s, sizeof(s), 3599);
    CHECK(!strcmp(s, "59 min"));
    sensors_format_age(s, sizeof(s), 3600 + 5 * 60 + 30);
    CHECK(!strcmp(s, "1 h 05 min"));
    sensors_format_age(s, sizeof(s), 86399);
    CHECK(!strcmp(s, "23 h 59 min"));
    sensors_format_age(s, sizeof(s), 2 * 86400 + 4 * 3600 + 59);
    CHECK(!strcmp(s, "2 d 04 h"));
}

TEST(lowest_and_highest) {
    sensors_stats_t st = {0};
    sensors_stats_add(&st, SENSORS_NONE, 5);       // ignored
    CHECK_EQ(st.count, 0);
    sensors_stats_add(&st, 300, 10);
    CHECK_EQ(st.count, 1);
    CHECK_EQ(st.min, 300);
    CHECK_EQ(st.max, 300);
    sensors_stats_add(&st, 280, 20);
    sensors_stats_add(&st, 320, 30);
    sensors_stats_add(&st, 280, 40);               // equal: keeps when it was first reached
    sensors_stats_add(&st, 310, 50);
    CHECK_EQ(st.count, 5);
    CHECK_EQ(st.min, 280);
    CHECK_EQ(st.min_s, 20);
    CHECK_EQ(st.max, 320);
    CHECK_EQ(st.max_s, 30);
    CHECK_EQ(st.last, 310);
    CHECK_EQ(st.last_s, 50);
    // Below zero counts as lower.
    sensors_stats_add(&st, -15, 60);
    CHECK_EQ(st.min, -15);
}

TEST(stats_shift) {
    sensors_stats_t st = {0};
    sensors_stats_shift(&st, 18);                  // nothing read yet: stays empty
    CHECK_EQ(st.count, 0);
    sensors_stats_add(&st, 250, 10);
    sensors_stats_add(&st, 270, 20);
    sensors_stats_add(&st, 260, 30);
    sensors_stats_shift(&st, 18);
    CHECK_EQ(st.min, 268);
    CHECK_EQ(st.max, 288);
    CHECK_EQ(st.last, 278);
    CHECK_EQ(st.min_s, 10);                        // the times stay
    CHECK_EQ(st.max_s, 20);
    sensors_stats_shift(&st, -18);
    CHECK_EQ(st.min, 250);
}

TEST(parse_temp) {
    int t = 999;
    CHECK(sensors_parse_temp("23", &t));
    CHECK_EQ(t, 230);
    CHECK(sensors_parse_temp("23.5", &t));
    CHECK_EQ(t, 235);
    CHECK(sensors_parse_temp("-4.2", &t));
    CHECK_EQ(t, -42);
    CHECK(sensors_parse_temp("+31.0", &t));
    CHECK_EQ(t, 310);
    CHECK(sensors_parse_temp("0", &t));
    CHECK_EQ(t, 0);
    CHECK(sensors_parse_temp("-0.5", &t));
    CHECK_EQ(t, -5);
    CHECK(sensors_parse_temp("-40", &t));
    CHECK_EQ(t, SENSORS_CAL_MIN);
    CHECK(sensors_parse_temp("125.0", &t));
    CHECK_EQ(t, SENSORS_CAL_MAX);
    t = 999;
    // Out of range, more than one decimal, junk: refused, and the value is untouched.
    const char *bad[] = {"", "-", "+", ".5", "23.", "23.45", "23,5", "23C", "23 ", " 23",
                         "125.1", "-40.1", "1e2", "99999999999", "abc", "--3"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(!sensors_parse_temp(bad[i], &t));
    CHECK_EQ(t, 999);
}

TEST(log_policy_temperature) {
    sensors_logged_t lg = {0};
    CHECK(!sensors_log_due(&lg, SENSORS_NONE, 4556, 1, 2));    // nothing without a temperature
    CHECK(sensors_log_due(&lg, 300, 4556, 1, 2));              // the first reading after boot
    sensors_log_mark(&lg, 300, 4556, 1, 2);
    CHECK(!sensors_log_due(&lg, 300, 4556, 1, 12));
    // Steady, or moving less than a degree: not before an hour.
    CHECK(!sensors_log_due(&lg, 309, 4556, 1, 600));
    CHECK(!sensors_log_due(&lg, 291, 4556, 1, 1200));
    CHECK(!sensors_log_due(&lg, 300, 4556, 1, 2 + SENSORS_LOG_MAX_S - 1));
    CHECK(sensors_log_due(&lg, 300, 4556, 1, 2 + SENSORS_LOG_MAX_S));
    sensors_log_mark(&lg, 300, 4556, 1, 2 + SENSORS_LOG_MAX_S);
    uint32_t t0 = lg.s;
    // A degree's change, but within a minute of the last line: waits.
    CHECK(!sensors_log_due(&lg, 310, 4556, 1, t0 + 10));
    CHECK(sensors_log_due(&lg, 310, 4556, 1, t0 + SENSORS_LOG_MIN_S));
    sensors_log_mark(&lg, 310, 4556, 1, t0 + SENSORS_LOG_MIN_S);
    CHECK_EQ(lg.temp, 310);
    // Down by a degree from the last line counts too.
    CHECK(sensors_log_due(&lg, 300, 4556, 1, lg.s + SENSORS_LOG_MIN_S));
}

TEST(log_policy_supply) {
    sensors_logged_t lg = {0};
    sensors_log_mark(&lg, 300, 4556, 1, 100);
    // A dip from a Wi-Fi transmission (0.13 V on dev-1) is below the step.
    CHECK(!sensors_log_due(&lg, 300, 4426, 1, 400));
    CHECK(!sensors_log_due(&lg, 300, 4556 - SENSORS_LOG_STEP_MV + 1, 1, 400));
    CHECK(sensors_log_due(&lg, 300, 4556 - SENSORS_LOG_STEP_MV, 1, 400));
    CHECK(sensors_log_due(&lg, 300, 4556 + SENSORS_LOG_STEP_MV, 1, 400));
    CHECK(!sensors_log_due(&lg, 300, 4556 + SENSORS_LOG_STEP_MV, 1, 159));  // within a minute
    // USB power coming or going: at once, even within the minute.
    CHECK(sensors_log_due(&lg, 300, 4556, 0, 101));
    sensors_log_mark(&lg, 300, 3900, 0, 101);
    CHECK(sensors_log_due(&lg, 300, 4556, 1, 102));
    // A supply reading appearing (the first line came before VSYS could be read) counts as a
    // change, after the minute.
    sensors_logged_t first = {0};
    sensors_log_mark(&first, 300, SENSORS_NONE, -1, 2);
    CHECK(sensors_log_due(&first, 300, 4556, -1, 2 + SENSORS_LOG_MIN_S));
    CHECK(!sensors_log_due(&first, 300, 4556, -1, 2 + SENSORS_LOG_MIN_S - 1));
}

TEST(scratch_register_form) {
    uint32_t t = SENSORS_MARK_TEMP, v = SENSORS_MARK_VSYS;
    CHECK_EQ(sensors_unpack(sensors_pack(312, t), t), 312);
    CHECK_EQ(sensors_unpack(sensors_pack(0, t), t), 0);
    CHECK_EQ(sensors_unpack(sensors_pack(-5, t), t), -5);
    CHECK_EQ(sensors_unpack(sensors_pack(-14803, t), t), -14803);
    CHECK_EQ(sensors_unpack(sensors_pack(INT16_MAX, t), t), INT16_MAX);
    CHECK_EQ(sensors_unpack(sensors_pack(SENSORS_NONE, t), t), SENSORS_NONE);
    CHECK_EQ(sensors_unpack(sensors_pack(4556, v), v), 4556);
    CHECK_EQ(sensors_unpack(sensors_pack(9898, v), v), 9898);
    CHECK_EQ(sensors_unpack(sensors_pack(SENSORS_NONE, v), v), SENSORS_NONE);
    // The temperature's form is the one #103 wrote: "CT" above the value.
    CHECK_EQ(sensors_pack(312, t), 0x43540138u);
    // Out of 16 bits: kept at the end of the range, never wrapped into a plausible reading.
    CHECK_EQ(sensors_unpack(sensors_pack(40000, t), t), INT16_MAX);
    CHECK_EQ(sensors_unpack(sensors_pack(-40000, t), t), SENSORS_NONE);
    // One reading's register is never taken for the other's.
    CHECK_EQ(sensors_unpack(sensors_pack(4556, v), t), SENSORS_NONE);
    CHECK_EQ(sensors_unpack(sensors_pack(312, t), v), SENSORS_NONE);
    // What a register holds at power-on, or the glitch detector's markers, is no reading.
    CHECK_EQ(sensors_unpack(0, t), SENSORS_NONE);
    CHECK_EQ(sensors_unpack(0x0000012c, v), SENSORS_NONE);
    CHECK_EQ(sensors_unpack(0x474c012c, t), SENSORS_NONE);
    CHECK_EQ(sensors_unpack(0x4753012c, v), SENSORS_NONE);
    CHECK_EQ(sensors_unpack(0xffffffff, t), SENSORS_NONE);
}

TEST_MAIN(
    RUN(temp_datasheet_example);
    RUN(temp_conversion);
    RUN(temp_negative);
    RUN(temp_format);
    RUN(vsys_conversion);
    RUN(vsys_format);
    RUN(format_both);
    RUN(format_age);
    RUN(lowest_and_highest);
    RUN(stats_shift);
    RUN(parse_temp);
    RUN(log_policy_temperature);
    RUN(log_policy_supply);
    RUN(scratch_register_form);
)
