// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// sos.date and sos.time's table form: see lua_date.h.

#include "lua/lua_date.h"

#include <stdbool.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "time/timeconv.h"

static const char *const wday_names[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                         "Thursday", "Friday", "Saturday"};
static const char *const month_names[] = {"January", "February", "March", "April", "May",
                                          "June", "July", "August", "September", "October",
                                          "November", "December"};

typedef struct {
    char *out;
    size_t cap, len;
    bool full;
} buf_t;

static void put(buf_t *b, const char *s, size_t n) {
    if (n >= b->cap - b->len) {                 // leave room for the NUL
        b->full = true;
        return;
    }
    memcpy(b->out + b->len, s, n);
    b->len += n;
}

static void puts_(buf_t *b, const char *s) {
    put(b, s, strlen(s));
}

// v in decimal, at least `width` characters, padded on the left with `pad`.
static void putnum(buf_t *b, long long v, int width, char pad) {
    char tmp[24], *end = tmp + sizeof(tmp), *p = end;
    unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
    do {
        *--p = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (v < 0)
        *--p = '-';
    while (end - p < width && p > tmp)
        *--p = pad;
    put(b, p, (size_t)(end - p));
}

static void put2(buf_t *b, int v) {
    putnum(b, v, 2, '0');
}

static int yday_of(const timeconv_cal_t *c) {
    return (int)(timeconv_days_from_civil(c->year, c->month, c->day) -
                 timeconv_days_from_civil(c->year, 1, 1)) + 1;
}

static bool format(buf_t *b, const char *fmt, const timeconv_cal_t *cal, int64_t t,
                   const char **bad);

// One conversion (the character after '%'); false if it isn't one this formatter knows.
// Composite ones are formats of their own (what the C locale gives).
static bool convert(buf_t *b, char c, const timeconv_cal_t *cal, int64_t t) {
    const char *sub = NULL;
    switch (c) {
    case 'a': put(b, wday_names[cal->wday], 3); break;
    case 'A': puts_(b, wday_names[cal->wday]); break;
    case 'b':
    case 'h': put(b, month_names[cal->month - 1], 3); break;
    case 'B': puts_(b, month_names[cal->month - 1]); break;
    case 'c': sub = "%a %b %e %H:%M:%S %Y"; break;
    case 'C': putnum(b, cal->year / 100, 1, '0'); break;    // as glibc: not padded
    case 'd': put2(b, cal->day); break;
    case 'D':
    case 'x': sub = "%m/%d/%y"; break;
    case 'e': putnum(b, cal->day, 2, ' '); break;
    case 'F': sub = "%Y-%m-%d"; break;
    case 'H': put2(b, cal->hour); break;
    case 'I': put2(b, cal->hour % 12 ? cal->hour % 12 : 12); break;
    case 'j': putnum(b, yday_of(cal), 3, '0'); break;
    case 'm': put2(b, cal->month); break;
    case 'M': put2(b, cal->min); break;
    case 'n': puts_(b, "\n"); break;
    case 'p': puts_(b, cal->hour < 12 ? "AM" : "PM"); break;
    case 'r': sub = "%I:%M:%S %p"; break;
    case 'R': sub = "%H:%M"; break;
    case 's': putnum(b, (long long)t, 1, '0'); break;
    case 'S': put2(b, cal->sec); break;
    case 't': puts_(b, "\t"); break;
    case 'T':
    case 'X': sub = "%H:%M:%S"; break;
    case 'u': putnum(b, cal->wday ? cal->wday : 7, 1, '0'); break;
    case 'w': putnum(b, cal->wday, 1, '0'); break;
    case 'y': put2(b, cal->year % 100); break;
    case 'Y': putnum(b, cal->year, 1, '0'); break;
    case 'z': puts_(b, "+0000"); break;
    case 'Z': puts_(b, "UTC"); break;
    case '%': puts_(b, "%"); break;
    default: return false;
    }
    const char *unused;
    return !sub || format(b, sub, cal, t, &unused);
}

static bool format(buf_t *b, const char *fmt, const timeconv_cal_t *cal, int64_t t,
                   const char **bad) {
    for (const char *p = fmt; *p && !b->full; p++) {
        if (*p != '%') {
            put(b, p, 1);
            continue;
        }
        if (!convert(b, p[1], cal, t)) {
            *bad = p;
            return false;
        }
        p++;
    }
    return true;
}

int lua_date_format(char *out, size_t cap, const char *fmt, int64_t t, const char **bad) {
    timeconv_cal_t cal;
    timeconv_to_cal(t, &cal);
    buf_t b = {out, cap, 0, cap == 0};
    if (!format(&b, fmt, &cal, t, bad))
        return -1;
    if (b.full) {
        *bad = fmt + strlen(fmt);
        return -1;
    }
    out[b.len] = '\0';
    return (int)b.len;
}

static void set_int(lua_State *L, const char *name, lua_Integer v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, name);
}

void lua_date_push(lua_State *L, const char *fmt, int64_t t) {
    if (t < LUA_DATE_MIN || t > LUA_DATE_MAX)
        luaL_error(L, "time out of range (years 1 to 9999)");
    if (*fmt == '!')
        fmt++;                                  // always UTC
    if (!strcmp(fmt, "*t")) {
        timeconv_cal_t c;
        timeconv_to_cal(t, &c);
        lua_createtable(L, 0, 9);
        set_int(L, "year", c.year);
        set_int(L, "month", c.month);
        set_int(L, "day", c.day);
        set_int(L, "hour", c.hour);
        set_int(L, "min", c.min);
        set_int(L, "sec", c.sec);
        set_int(L, "wday", c.wday + 1);
        set_int(L, "yday", yday_of(&c));
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "isdst");
        return;
    }
    char out[LUA_DATE_OUT_MAX];
    const char *bad;
    int n = lua_date_format(out, sizeof(out), fmt, t, &bad);
    if (n < 0) {
        if (*bad)
            luaL_error(L, "invalid conversion '%%%c' to 'sos.date'", bad[1] ? bad[1] : ' ');
        luaL_error(L, "date text longer than %d bytes", LUA_DATE_OUT_MAX - 1);
    }
    lua_pushlstring(L, out, (size_t)n);
}

// A field of the table at idx: an integer within +-1e9 (so the sums below can't overflow),
// or def (< 0: required).
static int field(lua_State *L, int idx, const char *name, int def) {
    int type = lua_getfield(L, idx, name);
    int ok = 0;
    lua_Integer v = lua_tointegerx(L, -1, &ok);
    lua_pop(L, 1);
    if (!ok) {
        if (type != LUA_TNIL)
            luaL_error(L, "field '%s' is not an integer", name);
        if (def < 0)
            luaL_error(L, "field '%s' missing in date table", name);
        return def;
    }
    if (v < -1000000000 || v > 1000000000)
        luaL_error(L, "field '%s' is out of range", name);
    return (int)v;
}

int64_t lua_date_from_table(lua_State *L, int idx) {
    luaL_checktype(L, idx, LUA_TTABLE);
    int year = field(L, idx, "year", -1), month = field(L, idx, "month", -1);
    int day = field(L, idx, "day", -1), hour = field(L, idx, "hour", 12);
    int min = field(L, idx, "min", 0), sec = field(L, idx, "sec", 0);
    if (year < 1 - 10000 || year > 10000 + 9999)
        luaL_error(L, "time out of range (years 1 to 9999)");
    int64_t t = timeconv_from_tm(year - 1900, month - 1, day, hour, min, sec);
    if (t < LUA_DATE_MIN || t > LUA_DATE_MAX)
        luaL_error(L, "time out of range (years 1 to 9999)");
    return t;
}
