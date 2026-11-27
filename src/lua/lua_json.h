// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// sos.json: JSON (RFC 8259) to Lua values and back, on core 1. Pure computation on a Lua state,
// so it runs in the host tests and under the fuzzer (tests/fuzz/fuzz_json.c).

#ifndef SOS_LUA_JSON_H
#define SOS_LUA_JSON_H

#include <stddef.h>

struct lua_State;

// Arrays and objects nest at most this deep, both ways: the C stack is core 1's 32 KB.
#define LUA_JSON_DEPTH 32

// Push sos.json.null: the value JSON's null decodes to (and encodes from), so that an array keeps
// its positions. A light userdata, the same one every time.
void lua_json_push_null(struct lua_State *L);

// Decode s[0..n). Pushes the value and returns 1, or pushes nil and a message (with the byte
// offset) and returns 2. Objects become tables with string keys, arrays tables from 1, numbers
// integers when written without a fraction or exponent and in range (floats otherwise), null
// sos.json.null. Raises only for out of memory.
int lua_json_decode(struct lua_State *L, const char *s, size_t n);

// Encode the value at idx and push the text. A table whose keys are exactly 1..n (n > 0) is an
// array; any other table an object, with its keys (strings, or integers written as text) in
// sorted order, so the same table always gives the same text. An empty table is {}. Raises a Lua
// error for what JSON can't hold: functions, NaN and infinities, other key types, nesting deeper
// than LUA_JSON_DEPTH (which also stops a table that contains itself).
void lua_json_encode(struct lua_State *L, int idx);

#endif
