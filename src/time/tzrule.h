// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// A time zone's rule, as a POSIX TZ string ("CET-1CEST,M3.5.0,M10.5.0/3"), and local time from
// it (#240). The board keeps UTC and shows local time only on screen; the rules come from
// tz/zones.bundle, the footer zic writes into each compiled IANA zone, so they say what a zone
// does now and from now on, not its history.
//
// The grammar is POSIX's with the extensions of RFC 9636 (TZif version 3) that zic uses:
//
//   std offset [dst [offset] ,start[/time],end[/time]]
//
//   std, dst  3 or more letters ("CET"), or <...> holding letters, digits, '+' and '-' ("<+1030>")
//   offset    [+|-]hh[:mm[:ss]], up to 24:00:00: what to ADD to local time to get UTC ("-1" is UTC+1)
//   start/end Jn (1-365, February 29 never counted), n (0-365, counted), or Mm.w.d (month 1-12,
//             week 1-5 where 5 is the last, weekday 0-6 with 0 Sunday)
//   time      [+|-]hh[:mm[:ss]], up to 167:00:00 either way: local time of the change, default 02:00
//
// A dst offset left out is std's plus one hour. A dst without the two dates is refused (POSIX
// leaves the dates to the implementation; zic always writes them).
//
// Pure C: no Pico SDK dependency, so it runs in the host tests and the fuzzer.

#ifndef SOS_TZRULE_H
#define SOS_TZRULE_H

#include <stdbool.h>
#include <stdint.h>

#define TZRULE_ABBR_MAX 15          // longest abbreviation kept ("<...>" without its brackets)

typedef struct {
    char kind;                      // 'J' (Jn), 'D' (n) or 'M' (Mm.w.d)
    int16_t day;                    // J: 1-365; D: 0-365; M: weekday 0-6
    int8_t month, week;             // M only: 1-12, 1-5
    int32_t time;                   // seconds after local midnight, -167 h to 167 h
} tzrule_change_t;

typedef struct {
    char std[TZRULE_ABBR_MAX + 1];
    char dst[TZRULE_ABBR_MAX + 1];  // empty: no daylight saving time
    int32_t std_off;                // seconds EAST of UTC (POSIX's sign turned round)
    int32_t dst_off;
    tzrule_change_t start, end;     // into and out of dst (local time before each change)
} tzrule_t;

// Parse the whole of s. False, with *why set when why isn't NULL, if s isn't a rule; *r is then
// UTC ("UTC", offset 0, no dst), so a caller showing local time falls back to UTC (and says so).
bool tzrule_parse(const char *s, tzrule_t *r, const char **why);

// Local time at t (seconds since 1970, UTC): the offset to add to t (seconds east of UTC),
// whether it is daylight saving time, and its abbreviation (points into r). Any t is accepted;
// beyond about 30,000 years from 1970 the result is std.
typedef struct {
    int32_t offset;
    bool dst;
    const char *abbr;
} tzrule_local_t;

tzrule_local_t tzrule_at(const tzrule_t *r, int64_t t);

// An offset as "+01:00", "-03:30", "+05:45:30" (seconds only when there are any) into buf (at
// least 10 bytes). Parsed offsets are within 24 hours; hours beyond 99 are shown as 99.
void tzrule_format_offset(int32_t offset, char *buf, unsigned buflen);

#endif
