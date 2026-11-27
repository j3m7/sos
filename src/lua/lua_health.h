// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Core 0's answers to a script's LR_INFO requests: read-only tables about the board (see
// src/lua/lua_health.c and docs/lua.md).

#ifndef SOS_LUA_HEALTH_H
#define SOS_LUA_HEALTH_H

#include <stddef.h>
#include <stdint.h>

#include "lua/lua_info.h"

typedef enum {
    LH_SENSORS,                 // sos.sensors()
    LH_MEM,                     // sos.mem()
    LH_VERSION,                 // sos.version()
    LH_RESET,                   // sos.reset_reason()
    LH_GLITCH,                  // sos.glitch()
    LH_WIFI,                    // sos.wifi.status()
    LH_CLOCK,                   // sos.clock()
    LH_SD,                      // sos.sd.info()
    LH_DMESG,                   // sos.dmesg(): one page of the log, from cursor `arg`
    LH_HEALTH,                  // sos.health()
    LH_TABLES
} lua_health_table_t;

// Write table `which` into buf (lua_info.h records); `arg` is its argument (LH_DMESG's cursor).
// Returns the bytes used, or 0 if it didn't fit or `which` is unknown.
size_t lua_health_fill(lua_health_table_t which, uint32_t arg, uint8_t *buf, size_t cap);

#endif
