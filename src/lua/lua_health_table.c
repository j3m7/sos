// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

// See lua_health_table.h. Moved from core/health.c (health_lua_table()), so that the core includes
// nothing from lua/ (docs/design/build-features.md §4.4). Pure C: host tested (tests/test_health.c).

#include "lua/lua_health_table.h"

#include <stdint.h>

#include "core/health.h"

void lua_health_table(lua_info_t *w) {
    health_status_t worst = health_worst();
    lua_info_str(w, "worst", health_status_name(worst));
    lua_info_int(w, "boot_ms", health_boot_ms());
    lua_info_open(w, "checks");
    uint32_t now = health_now_s();
    for (unsigned i = 0; i < health_count(); i++) {
        const health_entry_t *e = health_entry(i);
        lua_info_open(w, "");
        lua_info_str(w, "name", e->check->name);
        lua_info_str(w, "status", health_status_name(e->result.status));
        lua_info_str(w, "tier", health_tier_name(e->check->tier));
        if (e->result.status != HEALTH_NONE) {
            lua_info_str(w, "text", e->result.text);
            lua_info_int(w, "ago_s", now - e->at_s);
        }
        lua_info_int(w, "runs", e->runs);
        lua_info_int(w, "fails", e->fails);
        lua_info_close(w);
    }
    lua_info_close(w);
}
