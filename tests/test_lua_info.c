// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/lua/lua_info.c: the records core 0 writes for sos.sensors, sos.mem, ... and
// the Lua table core 1 makes of them, with a real Lua state.

#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lualib.h"
#include "lua/lua_info.h"
#include "test.h"

static lua_State *L;
static uint8_t buf[512];

// Run `check` (a Lua expression about the table `t`) against the table on top of the stack.
static bool holds(const char *check) {
    lua_setglobal(L, "t");
    char code[512];
    snprintf(code, sizeof(code), "return %s", check);
    if (luaL_dostring(L, code) != LUA_OK) {
        fprintf(stderr, "lua: %s\n", lua_tostring(L, -1));
        exit(1);
    }
    bool ok = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getglobal(L, "t");                      // back on top, for the next check
    return ok;
}

static size_t sample(size_t cap) {
    lua_info_t w;
    lua_info_begin(&w, buf, cap);
    lua_info_int(&w, "n", -42);
    lua_info_int(&w, "big", INT64_C(1) << 40);
    lua_info_num(&w, "temp", 23.5);
    lua_info_bool(&w, "yes", true);
    lua_info_bool(&w, "no", false);
    lua_info_str(&w, "s", "hello");
    lua_info_str(&w, "empty", "");
    lua_info_str(&w, "absent", NULL);
    lua_info_open(&w, "heap");
    lua_info_int(&w, "used", 100);
    lua_info_open(&w, "ssh/tls");
    lua_info_int(&w, "peak", 7);
    lua_info_close(&w);
    lua_info_close(&w);
    lua_info_str(&w, "after", "x");
    return lua_info_end(&w);
}

static void round_trip(void) {
    size_t n = sample(sizeof(buf));
    CHECK(n > 0);
    int top = lua_gettop(L);
    CHECK(lua_info_push(L, buf, n));
    CHECK_EQ(lua_gettop(L), top + 1);
    CHECK(holds("t.n == -42 and math.type(t.n) == 'integer'"));
    CHECK(holds("t.big == 1 << 40"));
    CHECK(holds("t.temp == 23.5 and math.type(t.temp) == 'float'"));
    CHECK(holds("t.yes == true and t.no == false"));
    CHECK(holds("t.s == 'hello' and t.empty == '' and t.absent == nil"));
    CHECK(holds("t.heap.used == 100 and t.heap['ssh/tls'].peak == 7"));
    CHECK(holds("t.after == 'x'"));             // back at the top level after the tables
    lua_settop(L, top);
}

static void empty_is_an_empty_table(void) {
    lua_info_t w;
    lua_info_begin(&w, buf, sizeof(buf));
    CHECK_EQ(lua_info_end(&w), 0);              // nothing written: 0 bytes...
    int top = lua_gettop(L);
    CHECK(lua_info_push(L, buf, 0));            // ...which reads as an empty table
    CHECK(holds("next(t) == nil"));
    lua_settop(L, top);
}

static void writer_refuses_what_doesnt_fit(void) {
    size_t full = sample(sizeof(buf));
    for (size_t cap = 0; cap < full; cap++)
        CHECK_EQ(sample(cap), 0);               // cut anywhere: nothing usable
    CHECK_EQ(sample(full), full);

    lua_info_t w;
    lua_info_begin(&w, buf, sizeof(buf));
    lua_info_open(&w, "a");
    CHECK_EQ(lua_info_end(&w), 0);              // a table left open

    lua_info_begin(&w, buf, sizeof(buf));
    lua_info_close(&w);
    CHECK_EQ(lua_info_end(&w), 0);              // closed without opening

    lua_info_begin(&w, buf, sizeof(buf));
    for (int i = 0; i <= LUA_INFO_DEPTH; i++)
        lua_info_open(&w, "d");
    for (int i = 0; i <= LUA_INFO_DEPTH; i++)
        lua_info_close(&w);
    CHECK_EQ(lua_info_end(&w), 0);              // too deep

    char name[300];
    memset(name, 'x', sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    lua_info_begin(&w, buf, sizeof(buf));
    lua_info_int(&w, name, 1);
    CHECK_EQ(lua_info_end(&w), 0);              // a name over 255 bytes
}

static void reader_refuses_malformed_records(void) {
    size_t full = sample(sizeof(buf));
    int top = lua_gettop(L);
    // A cut short version is refused, leaving the stack as it was, unless it falls between two
    // top-level records: then it's a well formed, shorter table.
    unsigned accepted = 0;
    for (size_t len = 0; len < full; len++) {
        if (lua_info_push(L, buf, len)) {
            accepted++;
            lua_settop(L, top);
        }
        CHECK_EQ(lua_gettop(L), top);
    }
    CHECK_EQ(accepted, 9u);     // 0 bytes, after each of the 7 records before "heap", and before "after"

    static const uint8_t unknown[] = {'x', 1, 'a'};
    CHECK(!lua_info_push(L, unknown, sizeof(unknown)));
    static const uint8_t stray_close[] = {'}'};
    CHECK(!lua_info_push(L, stray_close, sizeof(stray_close)));
    static const uint8_t name_too_long[] = {'b', 9, 'a', 1};
    CHECK(!lua_info_push(L, name_too_long, sizeof(name_too_long)));
    static const uint8_t string_too_long[] = {'s', 1, 'a', 9, 0, 'x'};
    CHECK(!lua_info_push(L, string_too_long, sizeof(string_too_long)));
    uint8_t deep[2 * 3 * (LUA_INFO_DEPTH + 1)];
    size_t n = 0;
    for (int i = 0; i <= LUA_INFO_DEPTH; i++) {
        deep[n++] = '{';
        deep[n++] = 1;
        deep[n++] = 'd';
    }
    for (int i = 0; i <= LUA_INFO_DEPTH; i++)
        deep[n++] = '}';
    CHECK(!lua_info_push(L, deep, n));
    CHECK_EQ(lua_gettop(L), top);
}

TEST_MAIN(
    L = luaL_newstate();
    luaL_requiref(L, "_G", luaopen_base, 1);
    luaL_requiref(L, "math", luaopen_math, 1);
    lua_pop(L, 2);
    RUN(round_trip);
    RUN(empty_is_an_empty_table);
    RUN(writer_refuses_what_doesnt_fit);
    RUN(reader_refuses_malformed_records);
    lua_close(L);
)
