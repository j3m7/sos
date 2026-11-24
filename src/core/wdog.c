// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "core/wdog.h"

#include "hardware/watchdog.h"

#if SOS_WATCHDOG_MS > 16777
#error "SOS_WATCHDOG_MS exceeds the RP2350 maximum of 16777 ms"
#endif

static bool timed_out;

void wdog_start(void) {
    // Must be read before watchdog_enable() rewrites scratch[4]. watchdog_reboot(0, 0, 0)
    // (the `reboot` command) clears it, so only real timeouts count.
    timed_out = watchdog_enable_caused_reboot();
#if SOS_WATCHDOG_MS
    watchdog_enable(SOS_WATCHDOG_MS, true);   // paused while a debugger halts the core
#endif
}

void wdog_feed(void) {
#if SOS_WATCHDOG_MS
    watchdog_update();
#endif
}

bool wdog_caused_reset(void) {
    return timed_out;
}
