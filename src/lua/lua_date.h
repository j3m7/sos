// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// sos.date and sos.time's table form, on core 1: like Lua's os.date and os.time, always in UTC
// (SOS has no time zones). Pure computation on time/timeconv.c, so it runs in the host tests.

#ifndef SOS_LUA_DATE_H
#define SOS_LUA_DATE_H

#include <stddef.h>
#include <stdint.h>

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

struct lua_State;

// sos.date's work once the time is known: with format "*t" (or "!*t"), push the table os.date
// gives ({year, month, day, hour, min, sec, wday, yday, isdst}, wday 1 = Sunday); otherwise push
// the formatted text (a leading "!" is accepted and changes nothing: it's always UTC). Raises a Lua
// error for an unknown conversion or a time outside LUA_DATE_MIN..MAX.
void lua_date_push(struct lua_State *L, const char *fmt, int64_t t);

// sos.time(table): the table at index idx as os.time takes it (year, month and day required; hour
// defaults to 12, min and sec to 0; fields out of range carry over, so month 13 is January of the
// next year). Raises a Lua error for a missing field or a time outside LUA_DATE_MIN..MAX.
int64_t lua_date_from_table(struct lua_State *L, int idx);

#endif
