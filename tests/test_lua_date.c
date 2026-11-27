// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/lua/lua_date.c: sos.date's formats checked against the C library's
// strftime() in UTC, and the Lua side (sos.date's "*t" table, sos.time(table)) with a real Lua
// state.

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lauxlib.h"
#include "lualib.h"
#include "lua/lua_date.h"
#include "test.h"
#include "time/timeconv.h"
#include "time/tzrule.h"

static lua_State *L;

// The conversions the C library formats the same way in the C locale and UTC (%Z says "GMT" in
// glibc, %s needs the time zone): each alone, and some formats mixing them with text.
static const char *const formats[] = {
    "%a", "%A", "%b", "%B", "%c", "%C", "%d", "%D", "%e", "%F", "%h", "%H", "%I", "%j",
    "%m", "%M", "%n", "%p", "%r", "%R", "%S", "%t", "%T", "%u", "%w", "%x", "%X", "%y",
    "%Y", "%z", "%%", LUA_DATE_DEFAULT, "day %j of %Y, %A %e %B, %I:%M %p", "",
};

static unsigned compared, mismatches;

static void compare(int64_t t) {
    time_t tt = (time_t)t;
    struct tm tm;
    if (!gmtime_r(&tt, &tm))
        return;
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        char want[256], got[256];
        const char *bad = NULL;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"    // the formats under test, as data
        size_t wn = strftime(want, sizeof(want), formats[i], &tm);
#pragma GCC diagnostic pop
        int gn = lua_date_format(got, sizeof(got), formats[i], t, &bad);
        compared++;
        if (gn < 0 || (size_t)gn != wn || strcmp(got, want)) {
            if (mismatches++ < 10)
                fprintf(stderr, "t=%lld '%s': got '%s', strftime '%s'\n", (long long)t,
                        formats[i], gn < 0 ? "(error)" : got, want);
        }
    }
}

static void formats_match_strftime(void) {
    static const int64_t edges[] = {
        0, -1, 1, 86399, 86400, 951782400 /* 2000-02-29 */, 951868800 /* 2000-03-01 */,
        -2203891200 /* 1900-03-01 */, 1798761599 /* 2026-12-31 23:59:59 */,
        1795996800 /* 2026-11-30 */, LUA_DATE_MIN, LUA_DATE_MAX, 4102444799 /* 2099-12-31 */,
        43200 /* noon: %p PM, %I 12 */, 0 + 3600 * 13, 4107542400 /* 2100-03-01 */,
    };
    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++)
        compare(edges[i]);
    srand(208);
    for (int i = 0; i < 3000; i++) {                     // anywhere in years 1 to 9999
        int64_t r = ((int64_t)rand() << 31) ^ rand();
        int64_t span = LUA_DATE_MAX - LUA_DATE_MIN + 1;
        int64_t t = LUA_DATE_MIN + (r % span + span) % span;
        compare(t);
    }
    for (int64_t t = 1767225600; t < 1767225600 + 400LL * 86400; t += 86400 + 3601)
        compare(t);                                     // every day (and hour) through 2026-27
    CHECK(compared > 100000);
    CHECK_EQ(mismatches, 0u);
}

static void format_errors(void) {
    char out[16];
    const char *bad = NULL;
    CHECK_EQ(lua_date_format(out, sizeof(out), "ok %Q", 0, &bad), -1);
    CHECK(bad && !strcmp(bad, "%Q"));
    CHECK_EQ(lua_date_format(out, sizeof(out), "%", 0, &bad), -1);       // a lone % at the end
    CHECK(bad && !strcmp(bad, "%"));
    CHECK_EQ(lua_date_format(out, sizeof(out), "%Y-%m-%d %H:%M:%S", 0, &bad), -1);   // 19 > 15
    CHECK(bad && *bad == '\0');
    CHECK_EQ(lua_date_format(out, sizeof(out), "%Y-%m-%d", 0, &bad), 10);
    CHECK(!strcmp(out, "1970-01-01"));
    CHECK_EQ(lua_date_format(out, sizeof(out), "%s", 1795996800, &bad), 10);
    CHECK(!strcmp(out, "1795996800"));
}

// --- in a time zone -------------------------------------------------------------------------

static const char *const zone_rules[] = {
    "CET-1CEST,M3.5.0,M10.5.0/3",           // Europe/Madrid
    "AEST-10AEDT,M10.1.0,M4.1.0/3",         // Australia/Sydney: dst across new year
    "EST5EDT,M3.2.0,M11.1.0",               // America/New_York
    "<+1030>-10:30<+11>-11,M10.1.0,M4.1.0", // Australia/Lord_Howe: half an hour
    "IST-1GMT0,M10.5.0,M3.5.0/1",           // Europe/Dublin: negative dst
    "<+0545>-5:45",                         // Asia/Kathmandu: no dst
};

static tzrule_t parsed(const char *text) {
    tzrule_t r;
    CHECK(tzrule_parse(text, &r, NULL));
    return r;
}

// Each conversion in each zone, against the C library with TZ set to the same rule, hourly
// through 2026 and 2027 and either side of each change.
static void formats_match_strftime_in_zones(void) {
    static const char *const fmts[] = {"%Y-%m-%d %H:%M:%S", "%z", "%Z", "%s", "%c %j %u %p"};
    unsigned n = 0, bad = 0;
    for (size_t z = 0; z < sizeof(zone_rules) / sizeof(zone_rules[0]); z++) {
        tzrule_t r = parsed(zone_rules[z]);
        setenv("TZ", zone_rules[z], 1);
        tzset();
        for (int64_t t = 1767225600; t < 1767225600 + 730LL * 86400; t += 1800) {
            time_t tt = (time_t)t;
            struct tm tm;
            localtime_r(&tt, &tm);
            for (size_t i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
                char want[128], got[128];
                const char *why = NULL;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
                strftime(want, sizeof(want), fmts[i], &tm);
#pragma GCC diagnostic pop
                int gn = lua_date_format_in(got, sizeof(got), fmts[i], t, &r, &why);
                n++;
                if (gn < 0 || strcmp(got, want)) {
                    if (bad++ < 10)
                        fprintf(stderr, "%s t=%lld '%s': got '%s', strftime '%s'\n",
                                zone_rules[z], (long long)t, fmts[i], got, want);
                }
            }
        }
    }
    setenv("TZ", "UTC0", 1);
    tzset();
    CHECK(n > 100000);
    CHECK_EQ(bad, 0u);
}

static int64_t at(int y, int mo, int d, int h, int mi) {
    timeconv_cal_t c = {.year = y, .month = mo, .day = d, .hour = h, .min = mi};
    int64_t t = 0;
    CHECK(timeconv_from_cal(&c, &t));
    return t;
}

// Wall times back to UTC: ordinary ones, the hour skipped in spring, the hour repeated in
// autumn, with and without isdst, and every half hour of a year in each zone.
static void wall_times(void) {
    tzrule_t madrid = parsed(zone_rules[0]);
    // 2026-03-29: 02:00 CET becomes 03:00 CEST (01:00 UTC).
    CHECK_EQ(lua_date_wall_to_utc(at(2026, 3, 29, 1, 30), &madrid, -1), at(2026, 3, 29, 0, 30));
    CHECK_EQ(lua_date_wall_to_utc(at(2026, 3, 29, 2, 30), &madrid, -1), at(2026, 3, 29, 1, 30));
    CHECK_EQ(lua_date_wall_to_utc(at(2026, 3, 29, 3, 30), &madrid, -1), at(2026, 3, 29, 1, 30));
    // 2026-10-25: 03:00 CEST becomes 02:00 CET (01:00 UTC); 02:30 comes twice.
    CHECK_EQ(lua_date_wall_to_utc(at(2026, 10, 25, 2, 30), &madrid, -1), at(2026, 10, 25, 0, 30));
    CHECK_EQ(lua_date_wall_to_utc(at(2026, 10, 25, 2, 30), &madrid, 1), at(2026, 10, 25, 0, 30));
    CHECK_EQ(lua_date_wall_to_utc(at(2026, 10, 25, 2, 30), &madrid, 0), at(2026, 10, 25, 1, 30));
    CHECK_EQ(lua_date_wall_to_utc(at(2026, 7, 1, 12, 0), &madrid, 0), at(2026, 7, 1, 10, 0));
    tzrule_t dublin = parsed(zone_rules[4]);     // winter is its "dst": GMT, offset 0
    CHECK_EQ(lua_date_wall_to_utc(at(2026, 3, 29, 1, 30), &dublin, -1), at(2026, 3, 29, 1, 30));
    CHECK_EQ(lua_date_wall_to_utc(at(2026, 10, 25, 1, 30), &dublin, -1), at(2026, 10, 25, 0, 30));
    CHECK_EQ(lua_date_wall_to_utc(at(2026, 10, 25, 1, 30), &dublin, 1), at(2026, 10, 25, 1, 30));
    CHECK_EQ(lua_date_wall_to_utc(at(2026, 1, 1, 0, 0), NULL, -1), at(2026, 1, 1, 0, 0));
    unsigned bad = 0;
    for (size_t z = 0; z < sizeof(zone_rules) / sizeof(zone_rules[0]); z++) {
        tzrule_t r = parsed(zone_rules[z]);
        for (int64_t t = 1767225600; t < 1767225600 + 366LL * 86400; t += 1800) {
            tzrule_local_t l = tzrule_at(&r, t);
            if (lua_date_wall_to_utc(t + l.offset, &r, l.dst) != t && bad++ < 10)
                fprintf(stderr, "%s t=%lld: no round trip\n", zone_rules[z], (long long)t);
        }
    }
    CHECK_EQ(bad, 0u);
}

// --- the Lua side ---------------------------------------------------------------------------

static tzrule_t zone;                           // the local functions' zone: Europe/Madrid

static int l_date(lua_State *S) {
    lua_date_push(S, luaL_optstring(S, 1, LUA_DATE_DEFAULT), luaL_checkinteger(S, 2), NULL);
    return 1;
}

static int l_time(lua_State *S) {
    lua_pushinteger(S, lua_date_from_table(S, 1, NULL));
    return 1;
}

static int l_localdate(lua_State *S) {
    lua_date_push(S, luaL_optstring(S, 1, LUA_DATE_DEFAULT), luaL_checkinteger(S, 2), &zone);
    return 1;
}

static int l_localtime(lua_State *S) {
    lua_pushinteger(S, lua_date_from_table(S, 1, &zone));
    return 1;
}

static bool holds(const char *code) {
    if (luaL_dostring(L, code) != LUA_OK) {
        fprintf(stderr, "lua: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    bool ok = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return ok;
}

// The error a chunk raises contains `text`.
static bool fails_with(const char *code, const char *text) {
    if (luaL_dostring(L, code) == LUA_OK) {
        lua_pop(L, lua_gettop(L));
        return false;
    }
    bool ok = strstr(lua_tostring(L, -1), text) != NULL;
    if (!ok)
        fprintf(stderr, "error was: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
    return ok;
}

static void date_table(void) {
    // 2026-11-29 14:03:05 UTC, a Sunday, day 333.
    CHECK(holds("local d = date('*t', 1795960985) return d.year == 2026 and d.month == 11 and "
                "d.day == 29 and d.hour == 14 and d.min == 3 and d.sec == 5"));
    CHECK(holds("local d = date('*t', 1795960985) return d.wday == 1 and d.yday == 333 and "
                "d.isdst == false and math.type(d.year) == 'integer'"));
    CHECK(holds("local a, b = date('*t', 1795960985), date('!*t', 1795960985) "
                "for k, v in pairs(a) do if b[k] ~= v then return false end end return true"));
    CHECK(holds("return date('!%Y', 0) == '1970' and date(nil, 0) == '1970-01-01 00:00:00'"));
    CHECK(holds("return date('*t', -1).year == 1969 and date('*t', -1).sec == 59"));
}

static void time_from_table(void) {
    CHECK(holds("return time{year = 2026, month = 11, day = 29, hour = 14, min = 3, sec = 5} "
                "== 1795960985"));
    CHECK(holds("return time{year = 1970, month = 1, day = 1} == 12 * 3600"));   // hour: 12
    CHECK(holds("return time{year = 2026, month = 13, day = 1, hour = 0} == "
                "time{year = 2027, month = 1, day = 1, hour = 0}"));             // carries over
    CHECK(holds("return time{year = 2026, month = 3, day = 0, hour = 0} == "
                "time{year = 2026, month = 2, day = 28, hour = 0}"));
    CHECK(holds("return time{year = 2026, month = 1, day = 1, hour = 0, min = -1} == "
                "time{year = 2025, month = 12, day = 31, hour = 23, min = 59}"));
    // A round trip from every few days across the range.
    CHECK(holds("for t = -62135596800, 253402300799, 86400 * 997 + 3607 do "
                "if time(date('*t', t)) ~= t then return false end end return true"));
}

static void local_from_lua(void) {
    // 2026-11-29 14:03:05 UTC is 15:03:05 CET; 2026-07-01 12:00 UTC is 14:00 CEST.
    CHECK(holds("return localdate(nil, 1795960985) == '2026-11-29 15:03:05' and "
                "localdate('%z %Z', 1795960985) == '+0100 CET' and "
                "localdate('%z %Z', 1782907200) == '+0200 CEST'"));
    CHECK(holds("return localdate('!%H', 1795960985) == '14' and "
                "date('%H %Z', 1795960985) == '14 UTC'"));                     // UTC unless asked
    CHECK(holds("local d = localdate('*t', 1782907200) return d.hour == 14 and d.isdst == true "
                "and localdate('*t', 1795960985).isdst == false"));
    CHECK(holds("return localtime{year = 2026, month = 11, day = 29, hour = 15, min = 3, sec = 5} "
                "== 1795960985"));
    CHECK(holds("local t = 1795960985 return localtime(localdate('*t', t)) == t"));
    CHECK(holds("local a = localtime{year = 2026, month = 10, day = 25, hour = 2, min = 30} "
                "local b = localtime{year = 2026, month = 10, day = 25, hour = 2, min = 30, "
                "isdst = false} return b - a == 3600"));                    // the repeated hour
    CHECK(holds("return localtime{year = 2026, month = 3, day = 29, hour = 2, min = 30} == "
                "localtime{year = 2026, month = 3, day = 29, hour = 3, min = 30}")); // skipped
    CHECK(holds("for t = 1767225600, 1798761600, 86400 * 3 + 1801 do "
                "if localtime(localdate('*t', t)) ~= t then return false end end return true"));
}

static void lua_errors(void) {
    CHECK(fails_with("return date('%Q', 0)", "invalid conversion '%Q'"));
    CHECK(fails_with("return date('%', 0)", "invalid conversion"));
    CHECK(fails_with("return date(string.rep('%c', 20), 0)", "longer than 255"));
    CHECK(fails_with("return date('*t', 253402300800)", "out of range"));
    CHECK(fails_with("return date('*t', -62135596801)", "out of range"));
    CHECK(fails_with("return time{month = 1, day = 1}", "field 'year' missing"));
    CHECK(fails_with("return time{year = 2026, month = 'x', day = 1}", "'month' is not an integer"));
    CHECK(fails_with("return time{year = 2026, month = 1, day = 1.5}", "'day' is not an integer"));
    CHECK(fails_with("return time{year = 10000, month = 1, day = 1}", "out of range"));
    CHECK(fails_with("return time{year = 2026, month = 1, day = 1, sec = 1e12}", "out of range"));
    CHECK(fails_with("return time(5)", "table expected"));
}

TEST_MAIN(
    setenv("TZ", "UTC0", 1);
    tzset();
    L = luaL_newstate();
    luaL_requiref(L, "_G", luaopen_base, 1);
    luaL_requiref(L, "string", luaopen_string, 1);
    luaL_requiref(L, "math", luaopen_math, 1);
    lua_pop(L, 3);
    lua_register(L, "date", l_date);
    lua_register(L, "time", l_time);
    lua_register(L, "localdate", l_localdate);
    lua_register(L, "localtime", l_localtime);
    zone = parsed(zone_rules[0]);
    RUN(formats_match_strftime);
    RUN(format_errors);
    RUN(date_table);
    RUN(time_from_table);
    RUN(lua_errors);
    RUN(formats_match_strftime_in_zones);
    RUN(wall_times);
    RUN(local_from_lua);
    lua_close(L);
)
