// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_FLASHOP_H
#define SOS_FLASHOP_H

#include <stdbool.h>
#include <stdint.h>

// Erasing or programming the flash the firmware runs from. The SDK's flash_safe_execute() stops
// the other core and turns interrupts off for as long as `fn` runs, so for that long nothing
// else is served: not the Wi-Fi driver, not USB. An erase takes tens of milliseconds. It is the
// longest time this firmware spends with interrupts off (issue #109), so every such write goes
// through here and is timed. `uptime` shows the figures.
//
// Main loop only. Returns true if `fn` ran.
bool flashop_run(void (*fn)(void *), void *param);

typedef struct {
    uint32_t count;         // writes since boot
    uint32_t last_us;       // how long the last one held interrupts off
    uint32_t max_us;        // the longest
} flashop_stats_t;

flashop_stats_t flashop_stats(void);

#endif
