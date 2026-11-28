// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The `core1` health check's reads (docs/design/health.md §9.5), moved from
// core/bgcheck_board.c (docs/design/build-features.md §4.3): the Lua job's heartbeat, which
// core/bgcheck.c judges.

#include "lua/lua_check.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "core/bgcheck.h"
#include "lua/lua_bridge.h"
#include "lua/lua_host.h"

_Static_assert((int)BGCHECK_WAIT_CORE0 == (int)LUA_WAIT_CORE0 &&
               (int)BGCHECK_WAIT_OUTPUT == (int)LUA_WAIT_OUTPUT &&
               (int)BGCHECK_WAIT_LINE == (int)LUA_WAIT_LINE, "the heartbeat's waits");

// --- core1 -------------------------------------------------------------------------------------------

static bgcheck_core1_state_t core1_state;

static const char *script_name(void) {
    if (lua_job.repl)
        return "lua prompt";
    if (lua_job.code)
        return "lua -e";
    const char *slash = strrchr(lua_job.path, '/');
    return slash ? slash + 1 : lua_job.path;
}

static void core1_gather(bgcheck_core1_in_t *in) {
    int st = lua_job.state;
    bool running = st == LJ_START || st == LJ_RUNNING;
    *in = (bgcheck_core1_in_t){
        .running = running,
        .cancel = lua_job.cancel,
        .waiting = running ? (bgcheck_wait_t)lua_beat.waiting : BGCHECK_WAIT_NONE,
        .beats = lua_beat.beats,
        .now_us = time_us_64(),
        .run_s = lua_host_job_seconds(),
        .name = running ? script_name() : NULL,
    };
}

health_step_t lua_check_core1_step(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (!cancel) {
        bgcheck_core1_in_t in;
        core1_gather(&in);
        bgcheck_judge_core1(&core1_state, &in, out);
    }
    return HEALTH_DONE;
}

#ifdef SOS_TEST_COMMANDS
#define DUMP_LINE 256

__attribute__((format(printf, 3, 4)))
static void dump(health_print_fn print, void *arg, const char *fmt, ...) {
    char line[DUMP_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    print(arg, line);
}

void lua_check_dump(health_print_fn print, void *arg) {
    bgcheck_core1_in_t c;
    core1_gather(&c);
    dump(print, arg, "core1: running %d, cancel %d, waiting %d, beats %lu, for %lu s, %s",
         c.running, c.cancel, c.waiting, (unsigned long)c.beats, (unsigned long)c.run_s,
         c.name ? c.name : "-");
}
#endif
