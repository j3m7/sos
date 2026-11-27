// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Tab at the Lua prompt: the names that could finish what's typed. Runs on core 1, in the
// session's Lua state, between lines. Uses only the Lua C API, so it's tested on the host with a
// real Lua state.

#ifndef SOS_LUA_COMPLETE_H
#define SOS_LUA_COMPLETE_H

#include <stdbool.h>
#include <stddef.h>

#include "lua.h"

// Characters of the expression Tab completes at the Lua prompt: names joined by '.' or ':'.
#define LUA_COMPLETE_CHARS \
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.:"

// The names that could finish `token`, a dotted name being typed ("string.fo", "t.", "s:up",
// "pri", or "" for the globals): each written whole into `out` ("string.format"), a NUL after
// each, `cap` bytes at most. Returns how many; *used gets the bytes written, *more is set if
// some didn't fit.
//
// The global table is walked with raw access only (lua_rawget, lua_next), so no metamethod runs:
// a Tab never runs the script's code. A value that isn't a table is looked through its
// metatable's __index when that is a table, so "s:up" finds the string methods; a table's own
// names come first, then those its metatable's __index table adds (one level: an __index
// chain isn't followed further). Only string keys that are
// names (letters, digits and '_', not a reserved word) are offered: anything else can't follow
// a '.'. Leaves the stack as it found it.
unsigned lua_complete(lua_State *L, const char *token, char *out, size_t cap, size_t *used,
                      bool *more);

#endif
