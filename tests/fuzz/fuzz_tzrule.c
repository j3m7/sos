// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// src/time/tzrule.c: a POSIX TZ rule, from tz/zones.bundle on the SD card or from the saved
// configuration (both can be edited by hand), and local time from it. Any input is parsed or
// refused; a parsed rule gives, at every hour of a year and at each change in it, one of its two
// offsets with the matching abbreviation.

#include <stdlib.h>
#include <string.h>

#include "fuzz.h"
#include "time/timeconv.h"
#include "time/tzrule.h"

// Checked in every build (NDEBUG or not).
#define MUST(x) do { if (!(x)) abort(); } while (0)

static void check_at(const tzrule_t *r, int64_t t) {
    tzrule_local_t l = tzrule_at(r, t);
    if (l.dst)
        MUST(r->dst[0] && l.offset == r->dst_off && l.abbr == r->dst);
    else
        MUST(l.offset == r->std_off && l.abbr == r->std);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // The first byte picks the year (1970 + 0..255); the rest is the rule, up to a NUL.
    int year = 1970 + fuzz_byte(&data, &size);
    char text[128];
    if (size >= sizeof(text))
        size = sizeof(text) - 1;
    memcpy(text, data, size);
    text[size] = '\0';

    tzrule_t r;
    const char *why = NULL;
    if (!tzrule_parse(text, &r, &why)) {
        MUST(why != NULL);
        return 0;
    }
    MUST(memchr(r.std, '\0', sizeof(r.std)) && strlen(r.std) >= 3);
    MUST(memchr(r.dst, '\0', sizeof(r.dst)));
    MUST(r.std_off >= -24 * 3600 && r.std_off <= 24 * 3600);
    MUST(r.dst_off >= -25 * 3600 && r.dst_off <= 25 * 3600);
    char buf[16];
    tzrule_format_offset(r.std_off, buf, sizeof(buf));
    MUST(buf[0] == '+' || buf[0] == '-');

    timeconv_cal_t jan1 = {.year = year, .month = 1, .day = 1};
    int64_t t0;
    MUST(timeconv_from_cal(&jan1, &t0));
    int64_t prev = tzrule_at(&r, t0 - 3600).offset;
    for (int64_t t = t0 - 3600; t < t0 + 367 * 86400; t += 3600) {
        check_at(&r, t);
        int32_t off = tzrule_at(&r, t + 3600).offset;
        if (off != prev) {                      // a change in this hour: the seconds around it
            for (int64_t s = t; s <= t + 3600; s += 60)
                check_at(&r, s);
            prev = off;
        }
    }
    check_at(&r, INT64_MIN);
    check_at(&r, INT64_MAX);
    return 0;
}
