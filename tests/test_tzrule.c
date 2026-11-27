// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/time/tzrule.c: POSIX TZ rules and local time from them. Besides fixed
// cases, every rule in tz/zones.bundle is checked against the host C library's own TZ handling
// (glibc reads the same POSIX strings, RFC 9636's extensions included) over five years, at each
// change and a second either side.

#define _DEFAULT_SOURCE     // setenv, tzset, localtime_r, tm_gmtoff, tm_zone
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "test.h"
#include "time/timeconv.h"
#include "time/tzrule.h"

static int64_t utc(int y, int mo, int d, int h, int mi, int s) {
    timeconv_cal_t cal = {.year = y, .month = mo, .day = d, .hour = h, .min = mi, .sec = s};
    int64_t t = 0;
    CHECK(timeconv_from_cal(&cal, &t));
    return t;
}

static tzrule_t rule(const char *s) {
    tzrule_t r;
    const char *why = NULL;
    bool ok = tzrule_parse(s, &r, &why);
    if (!ok)
        fprintf(stderr, "%s: %s\n", s, why);
    CHECK(ok);
    return r;
}

TEST(parse_fields) {
    tzrule_t r = rule("CET-1CEST,M3.5.0,M10.5.0/3");
    CHECK(!strcmp(r.std, "CET") && !strcmp(r.dst, "CEST"));
    CHECK_EQ(r.std_off, 3600);
    CHECK_EQ(r.dst_off, 7200);                              // left out: std + 1 h
    CHECK_EQ(r.start.kind, 'M');
    CHECK_EQ(r.start.month, 3);
    CHECK_EQ(r.start.week, 5);
    CHECK_EQ(r.start.day, 0);
    CHECK_EQ(r.start.time, 7200);                           // default 02:00
    CHECK_EQ(r.end.time, 3 * 3600);

    r = rule("<+1030>-10:30<+11>-11,M10.1.0,M4.1.0");
    CHECK(!strcmp(r.std, "+1030") && !strcmp(r.dst, "+11"));
    CHECK_EQ(r.std_off, 10 * 3600 + 1800);
    CHECK_EQ(r.dst_off, 11 * 3600);

    r = rule("<-02>2<-01>,M3.5.0/-1,M10.5.0/0");            // America/Nuuk: a negative time
    CHECK_EQ(r.std_off, -7200);
    CHECK_EQ(r.dst_off, -3600);
    CHECK_EQ(r.start.time, -3600);
    CHECK_EQ(r.end.time, 0);

    r = rule("EST5EDT,0/0,J365/25");                        // RFC 9636: DST all year
    CHECK_EQ(r.start.kind, 'D');
    CHECK_EQ(r.end.kind, 'J');
    CHECK_EQ(r.end.day, 365);
    CHECK_EQ(r.end.time, 25 * 3600);

    r = rule("UTC0");
    CHECK(!strcmp(r.std, "UTC") && !r.dst[0]);
    CHECK_EQ(r.std_off, 0);
    r = rule("<+0545>-5:45");
    CHECK_EQ(r.std_off, 5 * 3600 + 45 * 60);
    r = rule("LMT+0:25:21");
    CHECK_EQ(r.std_off, -(25 * 60 + 21));
    r = rule("AAA-24BBB+24,J1/167,J2/-167");                // every field at its limit
    CHECK_EQ(r.std_off, 24 * 3600);
    CHECK_EQ(r.dst_off, -24 * 3600);
    CHECK_EQ(r.start.time, 167 * 3600);
    CHECK_EQ(r.end.time, -167 * 3600);
}

static void at(const tzrule_t *r, int64_t t, int32_t offset, bool dst, const char *abbr);

TEST(parse_refuses) {
    static const char *const bad[] = {
        "", "C", "CE-1", "CET", "CET-", "CET-25", "CET-1:60", "CET-1:00:60", "CET-1:",
        "CET-1x", "CET-1CEST", "CET-1CEST,M3.5.0", "CET-1CEST,M3.5.0,", "CET-1CEST,M13.1.0,M1.1.0",
        "CET-1CEST,M3.0.0,M10.5.0", "CET-1CEST,M3.6.0,M10.5.0", "CET-1CEST,M3.5.7,M10.5.0",
        "CET-1CEST,M3.5,M10.5.0", "CET-1CEST,M3.5.0,M10.5.0/168", "CET-1CEST,M3.5.0,M10.5.0/",
        "CET-1CEST,J0,J365", "CET-1CEST,J366,J1", "CET-1CEST,366,1", "CET-1CEST,M3.5.0,M10.5.0x",
        "<+1>0", "<+1030-10", "<+10 30>-10", "ABCDEFGHIJKLMNOP0", "CET-1CEST,M3.5.0,M10.5.0,J1",
        "CET-0001", "CET-1CE,M3.5.0,M10.5.0", "CET--1", "CET-24:45", "CET-24:00:01",
        "CET-1CEST,M3.5.0,M10.5.0/167:00:01", "CET-1CEST,M3.5.0/-167:30,M10.5.0",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        tzrule_t r;
        const char *why = NULL;
        bool ok = tzrule_parse(bad[i], &r, &why);
        if (ok)
            fprintf(stderr, "accepted: '%s'\n", bad[i]);
        CHECK(!ok && why != NULL);
        at(&r, 1795476605, 0, false, "UTC");               // refused: UTC
    }
}

static void at(const tzrule_t *r, int64_t t, int32_t offset, bool dst, const char *abbr) {
    tzrule_local_t l = tzrule_at(r, t);
    CHECK_EQ(l.offset, offset);
    CHECK_EQ(l.dst, dst);
    CHECK(!strcmp(l.abbr, abbr));
}

TEST(known_changes) {
    tzrule_t madrid = rule("CET-1CEST,M3.5.0,M10.5.0/3");
    int64_t on = utc(2026, 3, 29, 1, 0, 0), off = utc(2026, 10, 25, 1, 0, 0);
    at(&madrid, on - 1, 3600, false, "CET");
    at(&madrid, on, 7200, true, "CEST");
    at(&madrid, off - 1, 7200, true, "CEST");
    at(&madrid, off, 3600, false, "CET");
    at(&madrid, utc(2026, 11, 27, 13, 3, 5), 3600, false, "CET");

    tzrule_t sydney = rule("AEST-10AEDT,M10.1.0,M4.1.0/3");  // southern: dst over new year
    at(&sydney, utc(2026, 4, 4, 15, 59, 59), 11 * 3600, true, "AEDT");
    at(&sydney, utc(2026, 4, 4, 16, 0, 0), 10 * 3600, false, "AEST");
    at(&sydney, utc(2026, 10, 3, 15, 59, 59), 10 * 3600, false, "AEST");
    at(&sydney, utc(2026, 10, 3, 16, 0, 0), 11 * 3600, true, "AEDT");
    at(&sydney, utc(2026, 12, 31, 13, 0, 0), 11 * 3600, true, "AEDT");   // already 2027 there
    at(&sydney, utc(2027, 1, 1, 0, 0, 0), 11 * 3600, true, "AEDT");

    tzrule_t ny = rule("EST5EDT,M3.2.0,M11.1.0");
    at(&ny, utc(2026, 3, 8, 6, 59, 59), -5 * 3600, false, "EST");
    at(&ny, utc(2026, 3, 8, 7, 0, 0), -4 * 3600, true, "EDT");
    at(&ny, utc(2026, 11, 1, 5, 59, 59), -4 * 3600, true, "EDT");
    at(&ny, utc(2026, 11, 1, 6, 0, 0), -5 * 3600, false, "EST");

    // Europe/Dublin: summer time (IST) is the standard one, and winter's GMT counts as the
    // daylight saving part, an hour behind it.
    tzrule_t dublin = rule("IST-1GMT0,M10.5.0,M3.5.0/1");
    CHECK_EQ(dublin.std_off, 3600);
    CHECK_EQ(dublin.dst_off, 0);
    at(&dublin, utc(2026, 7, 1, 12, 0, 0), 3600, false, "IST");
    at(&dublin, utc(2026, 12, 1, 12, 0, 0), 0, true, "GMT");
    at(&dublin, utc(2026, 3, 29, 0, 59, 59), 0, true, "GMT");
    at(&dublin, utc(2026, 3, 29, 1, 0, 0), 3600, false, "IST");
    at(&dublin, utc(2026, 10, 25, 0, 59, 59), 3600, false, "IST");
    at(&dublin, utc(2026, 10, 25, 1, 0, 0), 0, true, "GMT");

    tzrule_t tehran = rule("<+0330>-3:30");                 // a sign inside the brackets, no dst
    at(&tehran, utc(2026, 6, 1, 0, 0, 0), 3 * 3600 + 1800, false, "+0330");
    tzrule_t sao = rule("<-03>3");
    at(&sao, utc(2026, 1, 1, 0, 0, 0), -3 * 3600, false, "-03");

    tzrule_t always = rule("EST5EDT,0/0,J365/25");
    for (int y = 2026; y <= 2029; y++) {
        at(&always, utc(y, 1, 1, 0, 0, 0), -4 * 3600, true, "EDT");
        at(&always, utc(y, 7, 1, 0, 0, 0), -4 * 3600, true, "EDT");
        at(&always, utc(y, 12, 31, 23, 59, 59), -4 * 3600, true, "EDT");
    }

    tzrule_t utc0 = rule("UTC0");
    at(&utc0, 0, 0, false, "UTC");
    at(&utc0, INT64_MAX, 0, false, "UTC");
    at(&madrid, INT64_MIN, 3600, false, "CET");             // beyond the range: standard time
}

TEST(julian_days) {
    // J60 is March 1 in every year; 59 (counting from 0) is February 29 in a leap year.
    tzrule_t j = rule("AAA0BBB,J60/0,J300/0");
    at(&j, utc(2028, 2, 29, 23, 59, 59), 0, false, "AAA");
    at(&j, utc(2028, 3, 1, 0, 0, 0), 3600, true, "BBB");
    tzrule_t d = rule("AAA0BBB,59/0,300/0");
    at(&d, utc(2028, 2, 28, 23, 59, 59), 0, false, "AAA");
    at(&d, utc(2028, 2, 29, 0, 0, 0), 3600, true, "BBB");
    at(&d, utc(2027, 3, 1, 0, 0, 0), 3600, true, "BBB");    // day 59 of 2027 is March 1
    at(&d, utc(2027, 2, 28, 23, 59, 59), 0, false, "AAA");
}

TEST(format_offset) {
    char buf[16];
    tzrule_format_offset(3600, buf, sizeof(buf));
    CHECK(!strcmp(buf, "+01:00"));
    tzrule_format_offset(-12600, buf, sizeof(buf));
    CHECK(!strcmp(buf, "-03:30"));
    tzrule_format_offset(0, buf, sizeof(buf));
    CHECK(!strcmp(buf, "+00:00"));
    tzrule_format_offset(20730, buf, sizeof(buf));
    CHECK(!strcmp(buf, "+05:45:30"));
    tzrule_format_offset(INT32_MIN, buf, sizeof(buf));      // nonsense, but no overflow
    CHECK(buf[0] == '-');
}

// One instant under the C library with TZ set to the rule, against tzrule_at().
static bool agrees(const char *zone, const tzrule_t *r, int64_t t) {
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    tzrule_local_t l = tzrule_at(r, t);
    if (l.offset == tm.tm_gmtoff && l.dst == (tm.tm_isdst > 0) && !strcmp(l.abbr, tm.tm_zone))
        return true;
    fprintf(stderr, "%s at %lld: tzrule %d %d %s, libc %ld %d %s\n", zone, (long long)t,
            (int)l.offset, l.dst, l.abbr, (long)tm.tm_gmtoff, tm.tm_isdst, tm.tm_zone);
    return false;
}

TEST(bundle_against_libc) {
    FILE *f = fopen(TZ_BUNDLE, "r");
    CHECK(f != NULL);
    if (!f)
        return;
    char line[256];
    int zones = 0, failed = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#')
            continue;
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        char *sp = strchr(line, ' ');
        CHECK(sp != NULL);
        if (!sp)
            continue;
        *sp = '\0';
        const char *zone = line, *text = sp + 1;
        tzrule_t r;
        const char *why = NULL;
        if (!tzrule_parse(text, &r, &why)) {
            fprintf(stderr, "%s: '%s' refused: %s\n", zone, text, why);
            failed++;
            continue;
        }
        zones++;
        setenv("TZ", text, 1);
        tzset();
        bool ok = true;
        for (int y = 2026; ok && y < 2031; y++) {
            for (int64_t t = utc(y, 1, 1, 0, 0, 0); ok && t < utc(y + 1, 1, 1, 0, 0, 0);
                 t += 7 * 3600 + 13)
                ok = agrees(zone, &r, t);
            // Each change and a second either side.
            if (ok && r.dst[0]) {
                for (int m = 1; ok && m <= 12; m++) {
                        // Find each change in the month by hourly steps, then the second.
                        int64_t t0 = utc(y, m, 1, 0, 0, 0);
                        int64_t t1 = m == 12 ? utc(y + 1, 1, 1, 0, 0, 0) : utc(y, m + 1, 1, 0, 0, 0);
                        for (int64_t t = t0; ok && t < t1; t += 3600) {
                            if (tzrule_at(&r, t).offset == tzrule_at(&r, t + 3600).offset)
                                continue;
                            int64_t lo = t, hi = t + 3600;
                            while (hi - lo > 1) {
                                int64_t mid = lo + (hi - lo) / 2;
                                if (tzrule_at(&r, mid).offset == tzrule_at(&r, lo).offset)
                                    lo = mid;
                                else
                                    hi = mid;
                            }
                            ok = agrees(zone, &r, hi - 1) && agrees(zone, &r, hi) &&
                                 agrees(zone, &r, hi + 1);
                        }
                }
            }
        }
        if (!ok)
            failed++;
    }
    fclose(f);
    unsetenv("TZ");
    CHECK(zones > 500);
    CHECK_EQ(failed, 0);
}

TEST_MAIN(
    RUN(parse_fields);
    RUN(parse_refuses);
    RUN(known_changes);
    RUN(julian_days);
    RUN(format_offset);
    RUN(bundle_against_libc);
)
