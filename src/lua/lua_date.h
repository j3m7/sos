// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// sos.date and sos.time's table form, on core 1: like Lua's os.date and os.time, in UTC; and
// sos.localdate / sos.localtime, the same in the board's time zone (#240: a script gets local
// time only by asking for it). Pure computation on time/timeconv.c and time/tzrule.c, so it runs
// in the host tests.

#ifndef SOS_LUA_DATE_H
#define SOS_LUA_DATE_H

#include <stddef.h>
#include <stdint.h>

#include "time/tzrule.h"

// The times sos.date accepts: years 1 to 9999.
#define LUA_DATE_MIN INT64_C(-62135596800)     // 0001-01-01 00:00:00
#define LUA_DATE_MAX INT64_C(253402300799)     // 9999-12-31 23:59:59

// The format sos.date uses when none is given.
#define LUA_DATE_DEFAULT "%Y-%m-%d %H:%M:%S"

// The longest text sos.date makes; a format that would make more is refused.
#define LUA_DATE_OUT_MAX 256

// Format t (seconds since 1970, UTC) as strftime() does in the C locale, into out. The
// conversions are those of C99's strftime but %E, %O, %g, %G, %U, %V, %W, plus %s (the seconds).
// Returns the length, or -1 with *bad pointing at the conversion that isn't known (or at the end,
// for a text that doesn't fit in cap - 1 bytes).
int lua_date_format(char *out, size_t cap, const char *fmt, int64_t t, const char **bad);

// The same in a time zone (NULL: UTC): the fields are local time, %z the offset ("+0100"), %Z
// the abbreviation ("CET"); %s is still the UTC seconds.
int lua_date_format_in(char *out, size_t cap, const char *fmt, int64_t t, const tzrule_t *zone,
                       const char **bad);

// The UTC time of a wall time w (its calendar fields as seconds, as if it were UTC) in zone
// (NULL: UTC). A wall time that comes twice, when the clock goes back, is the earlier of the two
// unless isdst says which (1: daylight saving time, 0: standard, -1: not given); one that never
// comes, when the clock goes forward, is read with the offset before the change (02:30 on a
// night that skips from 02:00 to 03:00 is 03:30).
int64_t lua_date_wall_to_utc(int64_t w, const tzrule_t *zone, int isdst);

struct lua_State;

// sos.date's work once the time is known, in zone (NULL: UTC; a leading "!" in fmt also means
// UTC, as in os.date): with format "*t", push the table os.date gives ({year, month, day, hour,
// min, sec, wday, yday, isdst}, wday 1 = Sunday); otherwise push the formatted text. Raises a Lua
// error for an unknown conversion or a time outside LUA_DATE_MIN..MAX.
void lua_date_push(struct lua_State *L, const char *fmt, int64_t t, const tzrule_t *zone);

// sos.time(table): the table at index idx as os.time takes it (year, month and day required; hour
// defaults to 12, min and sec to 0; fields out of range carry over, so month 13 is January of the
// next year). In zone (NULL: UTC) the table is local time, and its isdst field, if any, picks
// between the two times of an hour that comes twice (lua_date_wall_to_utc). Raises a Lua error
// for a missing field or a time outside LUA_DATE_MIN..MAX.
int64_t lua_date_from_table(struct lua_State *L, int idx, const tzrule_t *zone);

#endif
