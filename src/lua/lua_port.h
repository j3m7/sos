// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Platform hooks for the Lua sources (force-included when they're compiled, see CMakeLists.txt).
// Lua runs on core 1, where stdio isn't usable: its own output goes through SOS's print().

#ifndef SOS_LUA_PORT_H
#define SOS_LUA_PORT_H

#include <stddef.h>

void sos_lua_write(const char *s, size_t len);     // src/lua/lua_core1.c: the script's output

#define lua_writestring(s, l)           sos_lua_write((s), (l))
#define lua_writeline()                 sos_lua_write("\n", 1)
#define lua_writestringerror(s, p)      ((void)(s), (void)(p))  // errors are returned instead

#define lua_getlocaledecpoint()         '.'

// Nested C calls (and parser nesting). Each level uses C stack; core 1's stack is 32 KB.
#define LUAI_MAXCCALLS                  120

#endif
