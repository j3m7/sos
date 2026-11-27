// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/lua/lua_complete.c: Tab at the Lua prompt, with a real Lua state.

#include <stdlib.h>

#include "lauxlib.h"
#include "lualib.h"
#include "lua/lua_complete.h"
#include "test.h"

static lua_State *L;
static char out[512];
static size_t used;
static bool more;
static unsigned n;

static void run(const char *code) {
    if (luaL_dostring(L, code) != LUA_OK) {
        fprintf(stderr, "lua: %s\n", lua_tostring(L, -1));
        exit(1);
    }
}

static unsigned complete(const char *token) {
    int top = lua_gettop(L);
    n = lua_complete(L, token, out, sizeof(out), &used, &more);
    CHECK_EQ(lua_gettop(L), top);               // the stack as it was
    return n;
}

// Is `name` among the names `out` holds?
static bool offered(const char *name) {
    for (size_t i = 0; i < used; i += strlen(out + i) + 1)
        if (!strcmp(out + i, name))
            return true;
    return false;
}

static void setup(void) {
    if (L)
        lua_close(L);
    L = luaL_newstate();
    luaL_requiref(L, "_G", luaopen_base, 1);
    luaL_requiref(L, "string", luaopen_string, 1);
    luaL_requiref(L, "table", luaopen_table, 1);
    lua_settop(L, 0);
}

TEST(globals_and_fields) {
    setup();
    CHECK_EQ(complete("pri"), 1);
    CHECK(offered("print"));
    CHECK(complete("string.fo") == 1 && offered("string.format"));
    CHECK(complete("string.") > 10 && offered("string.upper") && offered("string.sub"));
    CHECK(complete("") > 10 && offered("string") && offered("table") && offered("print"));
    CHECK_EQ(complete("nosuch.x"), 0);          // nil isn't a table
    CHECK_EQ(complete("print."), 0);            // nor a function
}

TEST(deeper_paths_and_methods) {
    setup();
    run("cfg = {net = {host = 'x', port = 22}, name = 'dev'} s = 'abc'");
    CHECK(complete("cfg.net.") == 2 && offered("cfg.net.host") && offered("cfg.net.port"));
    CHECK(complete("cfg.net.h") == 1 && offered("cfg.net.host"));
    CHECK(complete("s:up") == 1 && offered("s:upper"));      // a string: its methods
    CHECK(complete("cfg.name:le") == 1 && offered("cfg.name:len"));
    CHECK_EQ(complete("cfg:net.x"), 0);         // a ':' only before the last name
    CHECK_EQ(complete("cfg..net"), 0);
    CHECK_EQ(complete(".x"), 0);
    CHECK_EQ(complete(":x"), 0);
    CHECK_EQ(complete("1x."), 0);
    CHECK_EQ(complete("cfg.n-"), 0);
}

TEST(only_names_that_can_follow_a_dot) {
    setup();
    run("t = {ok = 1, ['two words'] = 2, ['end'] = 3, [1] = 4, ['\\27[2J'] = 5, _x = 6}");
    CHECK_EQ(complete("t."), 2);
    CHECK(offered("t.ok") && offered("t._x"));
}

TEST(no_code_runs) {
    setup();
    // A proxy whose __index and __pairs would run code: Tab must not call them.
    run("ran = 0 "
        "p = setmetatable({}, {__index = function() ran = ran + 1 return {} end, "
        "                      __pairs = function() ran = ran + 1 end})");
    CHECK_EQ(complete("p."), 0);
    CHECK_EQ(complete("p.x."), 0);              // no __index call to find x
    lua_getglobal(L, "ran");
    CHECK_EQ(lua_tointeger(L, -1), 0);
    lua_pop(L, 1);
}

TEST(an_objects_methods_through_its_metatable) {
    setup();
    run("Class = {greet = 1, size = 2} obj = setmetatable({size = 3, own = 4}, {__index = Class})");
    CHECK_EQ(complete("obj:"), 3);              // own, size, then greet (size not twice)
    CHECK(offered("obj:own") && offered("obj:size") && offered("obj:greet"));
}

TEST(output_is_bounded) {
    setup();
    run("big = {} for i = 1, 200 do big['name' .. i] = i end");
    n = lua_complete(L, "big.", out, 64, &used, &more);
    CHECK(more);
    CHECK(used <= 64);
    CHECK(n > 0 && n < 200);
    size_t seen = 0;
    for (size_t i = 0; i < used; i += strlen(out + i) + 1)
        seen++;
    CHECK_EQ(seen, n);
}

TEST_MAIN(
    RUN(globals_and_fields);
    RUN(deeper_paths_and_methods);
    RUN(only_names_that_can_follow_a_dot);
    RUN(no_code_runs);
    RUN(an_objects_methods_through_its_metatable);
    RUN(output_is_bounded);
    lua_close(L);
)
