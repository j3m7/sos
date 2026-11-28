// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_LUA_HEALTH_TABLE_H
#define SOS_LUA_HEALTH_TABLE_H

#include "lua/lua_info.h"

// The record sos.health() returns, from the health registry (core/health.h), written with
// lua_info.h's writer (a pure record format: no Lua here): {worst=, boot_ms=,
// checks={ {name=, status=, tier=, text=, ago_s=, runs=, fails=}, ... }}. A check that hasn't
// run has status "none" and no ago_s.
void lua_health_table(lua_info_t *w);

#endif
