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

// timeconv_from_tm() stands in for mktime() in UTC: the same answer as the C library's timegm(),
// for fields in range and for ones that carry (FatFs dates can hold month 0 or day 0).
TEST(from_tm_matches_timegm) {
    CHECK_EQ(timeconv_from_tm(70, 0, 1, 0, 0, 0), 0);
    CHECK_EQ(timeconv_from_tm(126, 10, 26, 12, 34, 56), 1795696496);   // 2026-11-26 12:34:56
    CHECK_EQ(timeconv_from_tm(80, 0, 0, 0, 0, 0), 315446400);          // day 0: 1979-12-31
    srand(4242);
    for (int i = 0; i < 200000; i++) {
        struct tm tm = {0};
        tm.tm_year = 70 + rand() % 140;
        tm.tm_mon = rand() % 40 - 14;       // out of range both ways
        tm.tm_mday = rand() % 80 - 20;
        tm.tm_hour = rand() % 60 - 10;
        tm.tm_min = rand() % 150 - 30;
        tm.tm_sec = rand() % 150 - 30;
        int64_t got = timeconv_from_tm(tm.tm_year, tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min,
                                       tm.tm_sec);
        if (got != (int64_t)timegm(&tm)) {  // timegm() normalizes tm: compare first
            CHECK_EQ(i, -1);                // report the failing round once and stop
            return;
        }
    }
    CHECK(1);
}

// src/time/utc_libc.c: what the firmware links in place of localtime_r() and mktime().
struct tm *__wrap_localtime_r(const time_t *t, struct tm *result);
time_t __wrap_mktime(struct tm *tm);

TEST(utc_replacements_for_localtime_and_mktime) {
    srand(777);
    for (int i = 0; i < 100000; i++) {
        time_t t = (time_t)(((uint64_t)rand() << 16 ^ (uint64_t)rand()) % 4102444800ULL);
        struct tm want, got;
        gmtime_r(&t, &want);
        CHECK(__wrap_localtime_r(&t, &got) == &got);
        if (got.tm_year != want.tm_year || got.tm_mon != want.tm_mon ||
            got.tm_mday != want.tm_mday || got.tm_hour != want.tm_hour ||
            got.tm_min != want.tm_min || got.tm_sec != want.tm_sec ||
            got.tm_wday != want.tm_wday || got.tm_yday != want.tm_yday) {
            CHECK_EQ(i, -1);
            return;
        }
        // mktime(): fields out of range are normalized in place, like timegm() does.
        struct tm a = want;
        a.tm_mon += rand() % 30 - 15;
        a.tm_mday += rand() % 90 - 45;
        a.tm_sec += rand() % 7200 - 3600;
        struct tm b = a;
        time_t ta = __wrap_mktime(&a), tb = timegm(&b);
        if (ta != tb || a.tm_year != b.tm_year || a.tm_mon != b.tm_mon ||
            a.tm_mday != b.tm_mday || a.tm_hour != b.tm_hour || a.tm_min != b.tm_min ||
            a.tm_sec != b.tm_sec || a.tm_wday != b.tm_wday || a.tm_yday != b.tm_yday) {
            CHECK_EQ(i, -2);
            return;
        }
    }
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
    CHECK(!timeconv_cal_steps(63 * 4340 + 1, &n));      // not rounded down into range
    CHECK(!timeconv_cal_steps(-64 * 4340 - 1, &n));
    CHECK(!timeconv_cal_steps(9999999, &n));
    CHECK_EQ(n, 5);
}

TEST_MAIN(
    RUN(known_dates);
    RUN(invalid_calendar);
    RUN(matches_libc);
    RUN(from_tm_matches_timegm);
    RUN(utc_replacements_for_localtime_and_mktime);
    RUN(rtc_registers);
    RUN(rtc_invalid_registers);
    RUN(rtc_round_trip);
    RUN(format);
    RUN(parse_ppm);
    RUN(cal_steps);
)
