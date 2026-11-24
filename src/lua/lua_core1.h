// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Starting the Lua interpreter on core 1 (see src/lua/lua_core1.c).

#ifndef SOS_LUA_CORE1_H
#define SOS_LUA_CORE1_H

#include <stdbool.h>
#include <stddef.h>

#define LUA_CORE1_STACK (32 * 1024)     // core 1's stack while it runs Lua (from the heap)

// Launch core 1 the first time (allocating its stack); true if it's running.
bool lua_core1_start(void);

// Deepest use of core 1's stack by the running or last script, in bytes; 0 before the first.
size_t lua_core1_stack_peak(void);

#endif
