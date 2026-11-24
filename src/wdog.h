// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_WDOG_H
#define SOS_WDOG_H

#include <stdbool.h>

// Hardware watchdog. Everything runs from the main loop, so only the main loop (and long boot
// steps such as the Wi-Fi connect) feed it: if the loop stops coming round for SOS_WATCHDOG_MS,
// the chip resets. SOS_WATCHDOG_MS=0 (CMake cache) disables it.

// Record why the last reset happened, then start the watchdog. Call once, early in main().
void wdog_start(void);

// Reload the watchdog counter.
void wdog_feed(void);

// True if the last reset was a watchdog timeout (not a `reboot` command or power-on).
bool wdog_caused_reset(void);

#endif
