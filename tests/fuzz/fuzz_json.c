// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Fuzz target for src/lua/lua_json.c (sos.json.decode reads what HTTPS servers send): any input
// decodes or is refused without crashing, and whatever decodes encodes, with encoding stable:
// encode(decode(encode(v))) == encode(v).

#include <stdlib.h>
#include <string.h>

#include "fuzz.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lua/lua_json.h"

static int encode(lua_State *L) {
    lua_json_encode(L, 1);
    return 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static lua_State *L;
    if (!L)
        L = luaL_newstate();
    lua_settop(L, 0);
    if (lua_json_decode(L, (const char *)data, size) != 1) {
        if (!lua_isnil(L, -2) || !lua_isstring(L, -1))
            abort();                            // a refusal is nil and a message
        return 0;
    }
    // Encoding what decoded mustn't fail; nor may encoding the result of decoding that.
    lua_pushcfunction(L, encode);
    lua_insert(L, 1);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK)
        abort();
    size_t n1;
    const char *first = lua_tolstring(L, -1, &n1);
    if (lua_json_decode(L, first, n1) != 1)
        abort();
    lua_pushcfunction(L, encode);
    lua_insert(L, -2);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK)
        abort();
    size_t n2;
    const char *second = lua_tolstring(L, -1, &n2);
    if (n1 != n2 || memcmp(first, second, n1))
        abort();
    lua_settop(L, 0);
    return 0;
}
