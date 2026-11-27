// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/lua/lua_json.c (sos.json), with a real Lua state: what decodes to what,
// what's refused and where, how tables encode, and round trips.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lualib.h"
#include "lua/lua_json.h"
#include "test.h"

static lua_State *L;

static int l_decode(lua_State *S) {
    size_t n;
    const char *s = luaL_checklstring(S, 1, &n);
    return lua_json_decode(S, s, n);
}

static int l_encode(lua_State *S) {
    luaL_checkany(S, 1);
    lua_json_encode(S, 1);
    return 1;
}

static bool holds(const char *code) {
    if (luaL_dostring(L, code) != LUA_OK) {
        fprintf(stderr, "lua: %s\n  in: %s\n", lua_tostring(L, -1), code);
        lua_settop(L, 0);
        return false;
    }
    bool ok = lua_toboolean(L, -1);
    if (!ok)
        fprintf(stderr, "false: %s\n", code);
    lua_settop(L, 0);
    return ok;
}

// Decoding `json` (a Lua string literal) fails with a message containing `text`.
static bool refused(const char *json, const char *text) {
    char code[600];
    snprintf(code, sizeof(code),
             "local v, err = json.decode(%s) return v == nil and err:find(%s, 1, true) ~= nil",
             json, text);
    if (!holds(code)) {
        snprintf(code, sizeof(code), "return select(2, json.decode(%s))", json);
        if (luaL_dostring(L, code) == LUA_OK)
            fprintf(stderr, "  got: %s\n", lua_tostring(L, -1));
        lua_settop(L, 0);
        return false;
    }
    return true;
}

static void decode_values(void) {
    CHECK(holds("return json.decode('true') == true and json.decode(' false ') == false"));
    CHECK(holds("return json.decode('null') == json.null"));
    CHECK(holds("return math.type(json.decode('123')) == 'integer' and json.decode('-7') == -7"));
    CHECK(holds("return json.decode('-0') == 0 and math.type(json.decode('0')) == 'integer'"));
    CHECK(holds("return json.decode('9223372036854775807') == math.maxinteger"));
    CHECK(holds("return json.decode('-9223372036854775808') == math.mininteger"));
    CHECK(holds("return math.type(json.decode('9223372036854775808')) == 'float'"));
    CHECK(holds("return json.decode('1.5') == 1.5 and json.decode('1e3') == 1000.0 "
                "and math.type(json.decode('1e3')) == 'float' and json.decode('-1.25E-2') == -0.0125"));
    CHECK(holds("return json.decode([[\"a\\\"b\\\\c\\/d\\b\\f\\n\\r\\t\"]]) == 'a\"b\\\\c/d\\b\\f\\n\\r\\t'"));
    CHECK(holds("return json.decode([[\"\\u00e9\\u20ac\"]]) == '\\xc3\\xa9\\xe2\\x82\\xac'"));
    CHECK(holds("return json.decode([[\"\\ud83d\\ude00\"]]) == '\\xf0\\x9f\\x98\\x80'"));
    CHECK(holds("return json.decode('\"caf\\xc3\\xa9\"') == 'caf\\xc3\\xa9'"));   // UTF-8 as is
    CHECK(holds("return json.decode([[\"\\u0000x\"]]) == '\\0x'"));
}

static void decode_structures(void) {
    CHECK(holds("local t = json.decode('{\"a\": [1, 2, {\"b\": null}], \"c\": {}}') "
                "return t.a[1] == 1 and t.a[3].b == json.null and next(t.c) == nil"));
    CHECK(holds("local t = json.decode('[1, null, 3]') return #t == 3 and t[2] == json.null"));
    CHECK(holds("local t = json.decode(' \\n[ ]\\t') return type(t) == 'table' and next(t) == nil"));
    CHECK(holds("local t = json.decode('{\"k\":1,\"k\":2}') return t.k == 2"));   // last wins
    CHECK(holds("return json.decode(string.rep('[', 32) .. string.rep(']', 32)) ~= nil"));
}

static void decode_refusals(void) {
    CHECK(refused("''", "'unexpected end at byte 1'"));
    CHECK(refused("'[1,]'", "'invalid value at byte 4'"));
    CHECK(refused("'{\"a\" 1}'", "\"expected ':' at byte 6\""));
    CHECK(refused("'{1:2}'", "'expected a string key at byte 2'"));
    CHECK(refused("'[1 2]'", "\"expected ',' or ']' at byte 4\""));
    CHECK(refused("'{\"a\":1'", "\"expected ',' or '}' at byte 7\""));
    CHECK(refused("'01'", "'trailing characters at byte 2'"));
    CHECK(refused("'[1] x'", "'trailing characters at byte 5'"));
    CHECK(refused("'1.'", "'invalid number'"));
    CHECK(refused("'.5'", "'invalid value'"));
    CHECK(refused("'+1'", "'invalid value'"));
    CHECK(refused("'1e'", "'invalid number'"));
    CHECK(refused("'-'", "'invalid value'"));
    CHECK(refused("'1e999'", "'number out of range'"));
    CHECK(refused("'NaN'", "'invalid value'"));
    CHECK(refused("'Infinity'", "'invalid value'"));
    CHECK(refused("'tru'", "'invalid value'"));
    CHECK(refused("'nul'", "'invalid value'"));
    CHECK(refused("'\"abc'", "'unfinished string'"));
    CHECK(refused("'\"a\\1b\"'", "'control character in string'"));
    CHECK(refused("[[\"\\x\"]]", "'invalid escape'"));
    CHECK(refused("[[\"\\u12\"]]", "'invalid escape'"));
    CHECK(refused("[[\"\\ud800\"]]", "'lone surrogate'"));
    CHECK(refused("[[\"\\ud800\\u0041\"]]", "'lone surrogate'"));
    CHECK(refused("[[\"\\udc00\"]]", "'lone surrogate'"));
    CHECK(refused("string.rep('[', 33) .. string.rep(']', 33)", "'nested too deep'"));
    CHECK(refused("'\"' .. string.rep('9', 70)", "'unfinished string'"));
    CHECK(holds("return math.type(json.decode(string.rep('9', 30) .. '.5')) == 'float'"));
    CHECK(refused("string.rep('9', 70) .. '.5'", "'number too long'"));
}

static void encode_values(void) {
    CHECK(holds("return json.encode(nil) == 'null' and json.encode(json.null) == 'null'"));
    CHECK(holds("return json.encode(true) == 'true' and json.encode(false) == 'false'"));
    CHECK(holds("return json.encode(42) == '42' and json.encode(-5) == '-5' "
                "and json.encode(math.mininteger) == '-9223372036854775808'"));
    CHECK(holds("return json.encode(1.5) == '1.5' and json.encode(1.0) == '1.0' "
                "and json.encode(1e300) == '1e+300'"));
    CHECK(holds("return json.encode('a\"b\\\\c\\n\\1\\127') == '\"a\\\\\"b\\\\\\\\c\\\\n\\\\u0001\\127\"'"));
    CHECK(holds("return json.encode('caf\\xc3\\xa9') == '\"caf\\xc3\\xa9\"'"));
}

static void encode_tables(void) {
    CHECK(holds("return json.encode({1, 2, 3}) == '[1,2,3]'"));
    CHECK(holds("return json.encode({1, json.null, 3}) == '[1,null,3]'"));
    CHECK(holds("return json.encode({b = 1, a = 2, ['10'] = 3}) == '{\"10\":3,\"a\":2,\"b\":1}'"));
    CHECK(holds("return json.encode({1, 2, x = 3}) == '{\"1\":1,\"2\":2,\"x\":3}'"));
    CHECK(holds("return json.encode({[1] = 1, [3] = 3}) == '{\"1\":1,\"3\":3}'"));
    CHECK(holds("return json.encode({[0] = 'z', 'a'}) == '{\"0\":\"z\",\"1\":\"a\"}'"));
    CHECK(holds("return json.encode({}) == '{}'"));
    CHECK(holds("return json.encode({a = {b = {1, {c = true}}}}) == '{\"a\":{\"b\":[1,{\"c\":true}]}}'"));
    CHECK(holds("local t = {} for i = 1, 3000 do t[i] = i end local s = json.encode(t) "
                "return #s > 10000 and s:sub(1, 7) == '[1,2,3,' and s:sub(-6) == ',3000]'"));
}

static void encode_refusals(void) {
    CHECK(holds("local ok, err = pcall(json.encode, print) return not ok and err:find('function')"));
    CHECK(holds("local ok, err = pcall(json.encode, {[{}] = 1}) return not ok and err:find('table key')"));
    CHECK(holds("local ok, err = pcall(json.encode, {[1.5] = 1}) return not ok and err:find('number key')"));
    CHECK(holds("local ok, err = pcall(json.encode, {[1] = 'a', ['1'] = 'b'}) "
                "return not ok and err:find('both \"1\"', 1, true)"));
    CHECK(holds("return json.encode({1, 2, nil, 4}) == '{\"1\":1,\"2\":2,\"4\":4}'"));
    CHECK(holds("local ok, err = pcall(json.encode, 0/0) return not ok and err:find('NaN')"));
    CHECK(holds("local ok, err = pcall(json.encode, 1/0) return not ok and err:find('infinity')"));
    CHECK(holds("local t = {} t.self = t local ok, err = pcall(json.encode, t) "
                "return not ok and err:find('contains itself')"));
    CHECK(holds("local ok = pcall(json.encode, json.decode(string.rep('[', 32) .. string.rep(']', 32))) "
                "return ok"));
}

static void round_trips(void) {
    static const char *const canonical[] = {
        "{\"a\":[1,2.5,\"x\",true,false,null],\"b\":{\"c\":{}}}",
        "[[],[[]],{\"\":\"\"},\"\\u0001\\\"\\\\\"]",
        "-12",
        "\"\\u00ff\"",
    };
    for (size_t i = 0; i < sizeof(canonical) / sizeof(canonical[0]); i++) {
        char code[400];
        snprintf(code, sizeof(code), "local s = [==[%s]==] local t = json.decode(s) "
                 "local again = json.encode(t) return again == s or again", canonical[i]);
        CHECK(holds(code) || (fprintf(stderr, "for %s\n", canonical[i]), false));
    }
    // "[]" can't survive: an empty table encodes as {} (lua-cjson's choice too).
    CHECK(holds("return json.encode(json.decode('[]')) == '{}'"));
    // A non-ASCII escape comes back as UTF-8 bytes, not as the escape.
    CHECK(holds("return json.encode(json.decode([[\"\\u00e9\"]])) == '\"\\xc3\\xa9\"'"));
}

TEST_MAIN(
    L = luaL_newstate();
    luaL_requiref(L, "_G", luaopen_base, 1);
    luaL_requiref(L, "string", luaopen_string, 1);
    luaL_requiref(L, "math", luaopen_math, 1);
    lua_pop(L, 3);
    lua_newtable(L);
    lua_pushcfunction(L, l_decode);
    lua_setfield(L, -2, "decode");
    lua_pushcfunction(L, l_encode);
    lua_setfield(L, -2, "encode");
    lua_json_push_null(L);
    lua_setfield(L, -2, "null");
    lua_setglobal(L, "json");
    RUN(decode_values);
    RUN(decode_structures);
    RUN(decode_refusals);
    RUN(encode_values);
    RUN(encode_tables);
    RUN(encode_refusals);
    RUN(round_trips);
    lua_close(L);
)
