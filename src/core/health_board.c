// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The firmware's health checks (core/health.h): the table, and steps that read what SOS already
// keeps. The judging is in health.c (pure, host tested); these only gather the numbers.
//
// Phase 1's seed checks (docs/design/health.md §9) read state only, take microseconds each, and
// hold no gate. Phase 1 has no full-tier check: `health full` runs the same checks as
// `health run`, plus the test-only gate checks of a test build.

#include "core/health.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "core/fault.h"
#include "core/glitchdet.h"
#include "core/heap.h"
#include "core/sensors.h"
#include "core/stackmon.h"
#include "core/wdog.h"
#include "lua/lua_core1.h"

#define SEED_PERIOD_S 60                // the background seed checks: once a minute

static health_step_t check_resets(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (!cancel)
        health_judge_resets(fault_last(), glitchdet_caused_reset(), wdog_caused_reset(), out);
    return HEALTH_DONE;
}

static health_step_t check_glitch(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (!cancel)
        health_judge_glitch(glitchdet_guard_tripped(), out);
    return HEALTH_DONE;
}

// stackmon_peak() scans core 0's painted stack for the deepest word changed: about 8 KB of
// reads, well under a millisecond.
static health_step_t check_stack(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (!cancel)
        health_judge_stack(stackmon_peak(), stackmon_size(), lua_core1_stack_peak(),
                           LUA_CORE1_STACK, out);
    return HEALTH_DONE;
}

static health_step_t check_heap(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (cancel)
        return HEALTH_DONE;
    int self = health_find("heap");
    const health_entry_t *e = self >= 0 ? health_entry((unsigned)self) : NULL;
    health_judge_heap(heap_free(), e ? e->result.status : HEALTH_NONE, out);
    return HEALTH_DONE;
}

// The latest reading (sensors_poll takes one every SENSORS_SAMPLE_S): no ADC access here.
static health_step_t check_temp(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (!cancel)
        health_judge_temp(sensors_last_temp(), out);
    return HEALTH_DONE;
}

// The lowest VSYS since the last run. Run again within SENSORS_SAMPLE_S (`health run` just after
// the background run) there is none new: the latest reading stands in.
static health_step_t check_supply(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (cancel)
        return HEALTH_DONE;
    int min = sensors_vsys_min_take();
    if (min == SENSORS_NONE)
        min = sensors_last_vsys();
    health_judge_supply(min, sensors_usb_power(), out);
    return HEALTH_DONE;
}

static const health_check_t checks[] = {
    {"resets", HEALTH_BOOT, 0, 0, 0, check_resets},
    {"glitch", HEALTH_BACKGROUND, SEED_PERIOD_S, 0, 0, check_glitch},
    {"stack", HEALTH_BACKGROUND, SEED_PERIOD_S, 0, 0, check_stack},
    {"heap", HEALTH_BACKGROUND, SEED_PERIOD_S, 0, 0, check_heap},
    {"chip-temp", HEALTH_BACKGROUND, SEED_PERIOD_S, 0, 0, check_temp},
    {"supply", HEALTH_BACKGROUND, SEED_PERIOD_S, 0, 0, check_supply},
};

void health_boot(void) {
    health_init(checks, sizeof(checks) / sizeof(checks[0]), time_us_64);
    health_run_boot();
}

#ifdef SOS_TEST_COMMANDS
// The checks `health inject gate` adds: each holds one gate, and is set to FAIL by hand. Run by
// `health full` (they are full-tier), they PASS, which opens the gate again.
static health_step_t check_injected(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (!cancel) {
        out->status = HEALTH_PASS;
        snprintf(out->text, sizeof(out->text), "test-only check (health inject gate)");
    }
    return HEALTH_DONE;
}

static const health_check_t inject_keys = {
    "inject-keys", HEALTH_FULL, 0, 0, HEALTH_GATE_KEYS, check_injected,
};
static const health_check_t inject_crypto = {
    "inject-crypto", HEALTH_FULL, 0, 0, HEALTH_GATE_CRYPTO, check_injected,
};

bool health_board_inject_gate(const char *gate) {
    const health_check_t *c = !strcmp(gate, "keys") ? &inject_keys
                            : !strcmp(gate, "crypto") ? &inject_crypto : NULL;
    if (!c || (health_find(c->name) < 0 && !health_add(c)))
        return false;
    return health_inject(c->name, HEALTH_FAIL, "set by hand (health inject gate)");
}
#endif
