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

// Nested C calls: each pcall, callback from C (string.gsub, table.sort, load), metamethod,
// coroutine and parser nesting level uses core 1's C stack (32 KB, LUA_CORE1_STACK). Plain
// Lua-to-Lua calls don't count. Measured per level on the board (`mem` after a script):
// string.gsub callback 1168 bytes, __tostring via string.format 968, pcall 520, coroutine 456,
// __index function 200, parser 128. A script starts at ~1.8 KB, and the traceback built at the
// deepest point takes ~1.6 KB more. 20 levels of the worst path, plus the 10% Lua allows while
// handling the overflow (20 / 10 * 11 = 22: keep it a multiple of 10, or integer division
// leaves no room and the traceback fails with "error in error handling"), stay under ~29 KB.
// The earlier 120 (Lua's default is 200) faulted the stack guard and reset the board instead of
// raising "C stack overflow". An error within a few levels of the limit can leave the traceback
// handler no room: Lua then reports "error in error handling", which src/lua/lua_core1.c reports
// as a C stack overflow.
#define LUAI_MAXCCALLS                  20

// require: modules come from the SD card (see src/lua/lua_core1.c, which replaces the file
// searchers). "./" is the shell's working directory, as for other script paths. No C modules.
#define LUA_PATH_DEFAULT                "/lib/?.lua;/lib/?/init.lua;./?.lua;./?/init.lua"
#define LUA_CPATH_DEFAULT               ""

#endif
