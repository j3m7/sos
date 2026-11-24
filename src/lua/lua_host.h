// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Lua scripts, run on core 1 (src/lua/lua_core1.c), as seen from core 0: the `lua` command and the
// main-loop hook that answers the script's requests.

#ifndef SOS_LUA_HOST_H
#define SOS_LUA_HOST_H

#include "shell/shell.h"

// `lua <file>` or `lua -e <code>`: run a script on core 1 as a job of this shell.
void lua_command(shell_t *sh, char *args);

// Answer the running script's requests; call from the main loop.
void lua_host_poll(void);

#endif
