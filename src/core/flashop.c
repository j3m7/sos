// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "core/flashop.h"

#include "pico/flash.h"
#include "pico/stdlib.h"

// How long to wait for the other core to stop before giving up.
#define LOCKOUT_TIMEOUT_MS 1000

typedef struct {
    void (*fn)(void *);
    void *param;
    uint32_t us;
} timed_t;

static flashop_stats_t stats;

// Runs with interrupts off and the other core stopped: the time measured is exactly that time.
// The timer is read from its registers and keeps counting meanwhile.
static void timed(void *arg) {
    timed_t *t = arg;
    uint64_t t0 = time_us_64();
    t->fn(t->param);
    t->us = (uint32_t)(time_us_64() - t0);
}

bool flashop_run(void (*fn)(void *), void *param) {
    timed_t t = { fn, param, 0 };
    if (flash_safe_execute(timed, &t, LOCKOUT_TIMEOUT_MS) != PICO_OK)
        return false;
    stats.count++;
    stats.last_us = t.us;
    if (t.us > stats.max_us)
        stats.max_us = t.us;
    return true;
}

flashop_stats_t flashop_stats(void) { return stats; }
