// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/time/timeconv.c: Unix time <-> calendar, and the PCF8523 register encoding.

#define _DEFAULT_SOURCE     // timegm, gmtime_r
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "test.h"
#include "time/timeconv.h"

static int64_t make(int y, int mo, int d, int h, int mi, int s) {
    timeconv_cal_t cal = {.year = y, .month = mo, .day = d, .hour = h, .min = mi, .sec = s};
    int64_t t = -1;
    CHECK(timeconv_from_cal(&cal, &t));
    return t;
}

TEST(known_dates) {
    CHECK_EQ(make(1970, 1, 1, 0, 0, 0), 0);
    CHECK_EQ(make(2000, 1, 1, 0, 0, 0), 946684800);
    CHECK_EQ(make(2000, 2, 29, 12, 0, 0), 951825600);         // leap day, century divisible by 400
    CHECK_EQ(make(2026, 11, 23, 23, 30, 5), 1795476605);
    CHECK_EQ(make(2038, 1, 19, 3, 14, 8), 2147483648LL);      // past 32-bit signed time_t
    CHECK_EQ(make(2099, 12, 31, 23, 59, 59), 4102444799LL);
    CHECK_EQ(make(1969, 12, 31, 23, 59, 59), -1);

    timeconv_cal_t cal;
    timeconv_to_cal(1795476605, &cal);
    CHECK_EQ(cal.year, 2026);
    CHECK_EQ(cal.month, 11);
    CHECK_EQ(cal.day, 23);
    CHECK_EQ(cal.hour, 23);
    CHECK_EQ(cal.min, 30);
    CHECK_EQ(cal.sec, 5);
    CHECK_EQ(cal.wday, 1);                                     // Monday
    timeconv_to_cal(0, &cal);
    CHECK_EQ(cal.wday, 4);                                     // Thursday
    timeconv_to_cal(-1, &cal);
    CHECK_EQ(cal.year, 1969);
    CHECK_EQ(cal.sec, 59);
    CHECK_EQ(cal.wday, 3);
}

TEST(invalid_calendar) {
    timeconv_cal_t ok = {.year = 2026, .month = 1, .day = 1};
    int64_t t;
    timeconv_cal_t c;
    c = ok; c.month = 0;  CHECK(!timeconv_from_cal(&c, &t));
    c = ok; c.month = 13; CHECK(!timeconv_from_cal(&c, &t));
    c = ok; c.day = 0;    CHECK(!timeconv_from_cal(&c, &t));
    c = ok; c.day = 32;   CHECK(!timeconv_from_cal(&c, &t));
    c = ok; c.month = 4; c.day = 31; CHECK(!timeconv_from_cal(&c, &t));
    c = ok; c.month = 2; c.day = 29; CHECK(!timeconv_from_cal(&c, &t));       // 2026 isn't leap
    c = ok; c.year = 2028; c.month = 2; c.day = 29; CHECK(timeconv_from_cal(&c, &t));
    c = ok; c.year = 2100; c.month = 2; c.day = 29; CHECK(!timeconv_from_cal(&c, &t));
    c = ok; c.hour = 24;  CHECK(!timeconv_from_cal(&c, &t));
    c = ok; c.min = 60;   CHECK(!timeconv_from_cal(&c, &t));
    c = ok; c.sec = 60;   CHECK(!timeconv_from_cal(&c, &t));
    c = ok; c.sec = -1;   CHECK(!timeconv_from_cal(&c, &t));
}

// Compare with the host C library over 1900..2200 at pseudo-random points.
TEST(matches_libc) {
    srand(12345);
    for (int i = 0; i < 200000; i++) {
        int64_t t = -2208988800LL + (int64_t)(((uint64_t)rand() << 31 | (uint64_t)rand()) %
                                              9467107200ULL);
        time_t tt = (time_t)t;
        struct tm tm;
        gmtime_r(&tt, &tm);
        timeconv_cal_t cal;
        timeconv_to_cal(t, &cal);
        if (cal.year != tm.tm_year + 1900 || cal.month != tm.tm_mon + 1 || cal.day != tm.tm_mday ||
            cal.hour != tm.tm_hour || cal.min != tm.tm_min || cal.sec != tm.tm_sec ||
            cal.wday != tm.tm_wday) {
            CHECK_EQ(t, -1);    // report the failing time once and stop
            return;
        }
        int64_t back;
        if (!timeconv_from_cal(&cal, &back) || back != t || back != (int64_t)timegm(&tm)) {
            CHECK_EQ(t, -2);
            return;
        }
    }
    CHECK(1);
}

TEST(rtc_registers) {
    uint8_t r[TIMECONV_RTC_REGS];
    CHECK(timeconv_to_rtc(1795476605, r));    // 2026-11-23 23:30:05, Monday
    const uint8_t want[TIMECONV_RTC_REGS] = {0x05, 0x30, 0x23, 0x23, 0x01, 0x11, 0x26};
    CHECK_MEM(r, want, sizeof(want));

    int64_t t;
    CHECK(timeconv_from_rtc(want, &t));
    CHECK_EQ(t, 1795476605);

    // The OS flag and unused high bits are ignored.
    uint8_t flagged[TIMECONV_RTC_REGS] = {0x85, 0xb0, 0xe3, 0xe3, 0xf9, 0xf1, 0x26};
    CHECK(timeconv_from_rtc(flagged, &t));
    CHECK_EQ(t, 1795476605);

    // Range: 2000-01-01 .. 2099-12-31.
    CHECK(timeconv_to_rtc(946684800, r));
    CHECK(!timeconv_to_rtc(946684799, r));
    CHECK(timeconv_to_rtc(4102444799LL, r));
    CHECK(!timeconv_to_rtc(4102444800LL, r));
}

TEST(rtc_invalid_registers) {
    const uint8_t ok[TIMECONV_RTC_REGS] = {0x05, 0x30, 0x23, 0x23, 0x01, 0x11, 0x26};
    int64_t t;
    uint8_t r[TIMECONV_RTC_REGS];
    memcpy(r, ok, sizeof(r)); r[0] = 0x0a; CHECK(!timeconv_from_rtc(r, &t));   // not BCD
    memcpy(r, ok, sizeof(r)); r[0] = 0x60; CHECK(!timeconv_from_rtc(r, &t));   // 60 s
    memcpy(r, ok, sizeof(r)); r[2] = 0x24; CHECK(!timeconv_from_rtc(r, &t));   // 24 h
    memcpy(r, ok, sizeof(r)); r[3] = 0x00; CHECK(!timeconv_from_rtc(r, &t));   // day 0
    memcpy(r, ok, sizeof(r)); r[3] = 0x31; CHECK(!timeconv_from_rtc(r, &t));   // 31 November
    memcpy(r, ok, sizeof(r)); r[5] = 0x13; CHECK(!timeconv_from_rtc(r, &t));   // month 13
    memcpy(r, ok, sizeof(r)); r[5] = 0x00; CHECK(!timeconv_from_rtc(r, &t));   // month 0
    memcpy(r, ok, sizeof(r)); r[6] = 0xa0; CHECK(!timeconv_from_rtc(r, &t));   // not BCD
    // Erased/floating bus reads as 0xff everywhere.
    memset(r, 0xff, sizeof(r)); CHECK(!timeconv_from_rtc(r, &t));
}

TEST(rtc_round_trip) {
    srand(678);
    for (int i = 0; i < 100000; i++) {
        int64_t t = 946684800LL + (int64_t)(((uint64_t)rand() << 31 | (uint64_t)rand()) %
                                            3155760000ULL);     // 2000..2099
        uint8_t r[TIMECONV_RTC_REGS];
        int64_t back;
        if (!timeconv_to_rtc(t, r) || !timeconv_from_rtc(r, &back) || back != t) {
            CHECK_EQ(t, -1);
            return;
        }
    }
    CHECK(1);
}

TEST(format) {
    char buf[24];
    timeconv_format(1795476605, buf, sizeof(buf));
    CHECK(!strcmp(buf, "2026-11-23 23:30:05"));
    timeconv_format(0, buf, sizeof(buf));
    CHECK(!strcmp(buf, "1970-01-01 00:00:00"));
}

TEST(parse_ppm) {
    int32_t v = 1;
    CHECK(timeconv_parse_ppm("92", &v));        CHECK_EQ(v, 92000);
    CHECK(timeconv_parse_ppm("+92", &v));       CHECK_EQ(v, 92000);
    CHECK(timeconv_parse_ppm("-12.5", &v));     CHECK_EQ(v, -12500);
    CHECK(timeconv_parse_ppm("0", &v));         CHECK_EQ(v, 0);
    CHECK(timeconv_parse_ppm("0.004", &v));     CHECK_EQ(v, 4);
    CHECK(timeconv_parse_ppm("9999.999", &v));  CHECK_EQ(v, 9999999);
    v = 7;
    const char *bad[] = {"", "+", "-", ".5", "5.", "1.2345", "12345", "9 2", "92ppm", "1e2",
                         "--1", "0x10"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(!timeconv_parse_ppm(bad[i], &v));
    CHECK_EQ(v, 7);                             // untouched on failure
}

TEST(cal_steps) {
    int n = 99;
    CHECK(timeconv_cal_steps(0, &n));           CHECK_EQ(n, 0);
    CHECK(timeconv_cal_steps(92000, &n));       CHECK_EQ(n, 21);     // 21 * 4.34 = 91.14
    CHECK(timeconv_cal_steps(-92000, &n));      CHECK_EQ(n, -21);
    CHECK(timeconv_cal_steps(2169, &n));        CHECK_EQ(n, 0);      // under half a step
    CHECK(timeconv_cal_steps(2170, &n));        CHECK_EQ(n, 1);      // half a step rounds away
    CHECK(timeconv_cal_steps(-2170, &n));       CHECK_EQ(n, -1);
    // The register's range: -64..63 steps.
    CHECK(timeconv_cal_steps(63 * 4340, &n));   CHECK_EQ(n, 63);
    CHECK(timeconv_cal_steps(-64 * 4340, &n));  CHECK_EQ(n, -64);
    n = 5;
    CHECK(!timeconv_cal_steps(63 * 4340 + 2170, &n));
    CHECK(!timeconv_cal_steps(-64 * 4340 - 2170, &n));
    CHECK(!timeconv_cal_steps(9999999, &n));
    CHECK_EQ(n, 5);
}

TEST_MAIN(
    RUN(known_dates);
    RUN(invalid_calendar);
    RUN(matches_libc);
    RUN(rtc_registers);
    RUN(rtc_invalid_registers);
    RUN(rtc_round_trip);
    RUN(format);
    RUN(parse_ppm);
    RUN(cal_steps);
)
