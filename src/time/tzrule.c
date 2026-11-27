// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "time/tzrule.h"

#include <stdio.h>
#include <string.h>

#include "time/timeconv.h"

#define HOUR 3600

static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

// Up to `max_digits` digits into *v, at least one; false if none.
static bool number(const char **p, int max_digits, int *v) {
    int n = 0, digits = 0;
    while (is_digit(**p) && digits < max_digits) {
        n = n * 10 + (**p - '0');
        (*p)++;
        digits++;
    }
    *v = n;
    return digits > 0 && !is_digit(**p);
}

// An abbreviation: 3 or more letters, or <...> of letters, digits, '+' and '-'.
static bool abbr(const char **p, char out[TZRULE_ABBR_MAX + 1]) {
    const char *s = *p;
    size_t n = 0;
    if (*s == '<') {
        s++;
        while (is_alpha(s[n]) || is_digit(s[n]) || s[n] == '+' || s[n] == '-')
            n++;
        if (s[n] != '>')
            return false;
        *p = s + n + 1;
    } else {
        while (is_alpha(s[n]))
            n++;
        *p = s + n;
    }
    if (n < 3 || n > TZRULE_ABBR_MAX)
        return false;
    memcpy(out, s, n);
    out[n] = '\0';
    return true;
}

// [+|-]hh[:mm[:ss]], at most max_hours:00:00 either way, as seconds.
static bool hms(const char **p, int max_hours, int32_t *secs) {
    int sign = 1, h, m = 0, s = 0;
    if (**p == '+' || **p == '-') {
        sign = **p == '-' ? -1 : 1;
        (*p)++;
    }
    if (!number(p, 3, &h) || h > max_hours)
        return false;
    if (**p == ':') {
        (*p)++;
        if (!number(p, 2, &m) || m > 59)
            return false;
        if (**p == ':') {
            (*p)++;
            if (!number(p, 2, &s) || s > 59)
                return false;
        }
    }
    int total = h * HOUR + m * 60 + s;
    if (total > max_hours * HOUR)               // "24:30" is past 24 hours
        return false;
    *secs = sign * total;
    return true;
}

// A '.', stepped over; false (and not moved) if it isn't one.
static bool dot(const char **p) {
    if (**p != '.')
        return false;
    (*p)++;
    return true;
}

// ,date[/time]
static bool change(const char **p, tzrule_change_t *c) {
    int v;
    if (**p != ',')
        return false;
    (*p)++;
    memset(c, 0, sizeof(*c));
    if (**p == 'J') {
        (*p)++;
        if (!number(p, 3, &v) || v < 1 || v > 365)
            return false;
        c->kind = 'J';
        c->day = (int16_t)v;
    } else if (**p == 'M') {
        int w, d;
        (*p)++;
        if (!number(p, 2, &v) || v < 1 || v > 12 || !dot(p) ||
            !number(p, 1, &w) || w < 1 || w > 5 || !dot(p) ||
            !number(p, 1, &d) || d > 6)
            return false;
        c->kind = 'M';
        c->month = (int8_t)v;
        c->week = (int8_t)w;
        c->day = (int16_t)d;
    } else {
        if (!number(p, 3, &v) || v > 365)
            return false;
        c->kind = 'D';
        c->day = (int16_t)v;
    }
    c->time = 2 * HOUR;
    if (**p == '/') {
        (*p)++;
        if (!hms(p, 167, &c->time))
            return false;
    }
    return true;
}

bool tzrule_parse(const char *s, tzrule_t *r, const char **why) {
    const char *w = NULL;
    int32_t off;
    memset(r, 0, sizeof(*r));
    if (!abbr(&s, r->std))
        w = "bad standard time name";
    else if (!hms(&s, 24, &off))
        w = "bad standard time offset";
    else {
        r->std_off = r->dst_off = -off;
        if (*s) {
            r->dst_off = r->std_off + HOUR;         // unless an offset follows the name
            if (!abbr(&s, r->dst))
                w = "bad daylight saving time name";
            else if (*s != ',') {
                if (hms(&s, 24, &off))
                    r->dst_off = -off;
                else
                    w = "bad daylight saving time offset";
            }
            if (!w && (!change(&s, &r->start) || !change(&s, &r->end)))
                w = "bad or missing change dates";
        }
        if (!w && *s)
            w = "unexpected text after the rule";
    }
    if (w) {
        memset(r, 0, sizeof(*r));
        memcpy(r->std, "UTC", 4);               // refused: UTC, so a caller that goes on anyway
        if (why)
            *why = w;
        return false;
    }
    return true;
}

static bool leap(int64_t y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }

// The change in year y, in UTC, given the offset in force just before it.
static int64_t change_utc(const tzrule_change_t *c, int y, int32_t before) {
    int64_t days;                   // since 1970 of the change's day
    int64_t jan1 = timeconv_days_from_civil(y, 1, 1);
    if (c->kind == 'J') {
        days = jan1 + c->day - 1 + (leap(y) && c->day >= 60);
    } else if (c->kind == 'D') {
        days = jan1 + c->day;
    } else {
        int64_t first = timeconv_days_from_civil(y, c->month, 1);
        int64_t wd = (first + 4) % 7;                       // 1970-01-01 was a Thursday
        if (wd < 0)
            wd += 7;
        int64_t d = (c->day - wd + 7) % 7 + 7 * (c->week - 1);   // 0-based day in the month
        int64_t next = c->month == 12 ? timeconv_days_from_civil(y + 1, 1, 1)
                                      : timeconv_days_from_civil(y, c->month + 1, 1);
        while (first + d >= next)
            d -= 7;                                         // week 5: the last one
        days = first + d;
    }
    return days * 86400 + c->time - before;
}

tzrule_local_t tzrule_at(const tzrule_t *r, int64_t t) {
    tzrule_local_t std = {r->std_off, false, r->std};
    tzrule_local_t dst = {r->dst_off, true, r->dst};
    if (!r->dst[0] || t < -(INT64_C(1) << 40) || t > (INT64_C(1) << 40))
        return std;
    timeconv_cal_t cal;
    timeconv_to_cal(t + r->std_off, &cal);                  // the year, as standard time has it
    int64_t start = change_utc(&r->start, cal.year, r->std_off);
    int64_t end = change_utc(&r->end, cal.year, r->dst_off);
    if (start < end)                                        // northern: dst inside the year
        return t >= start && t < end ? dst : std;
    return t >= end && t < start ? std : dst;               // southern: dst across new year
}

void tzrule_format_offset(int32_t offset, char *buf, unsigned buflen) {
    char sign = offset < 0 ? '-' : '+';
    uint32_t a = offset < 0 ? (uint32_t)-(int64_t)offset : (uint32_t)offset;
    unsigned h = a / 3600 > 99 ? 99 : a / 3600, m = a / 60 % 60, s = a % 60;   // parsed: <= 24
    if (s)
        snprintf(buf, buflen, "%c%02u:%02u:%02u", sign, h, m, s);
    else
        snprintf(buf, buflen, "%c%02u:%02u", sign, h, m);
}
