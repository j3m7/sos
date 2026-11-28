// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The CYW43 driver's errors, counted since boot (docs/design/health.md §9.9). The driver reports
// them through two macros its port defines, CYW43_FAIL_FAST_CHECK and CYW43_WARN, which
// config/cyw43_sos.h (CYW43_CONFIG_FILE) points at wifistat_fail() and wifistat_warn(): no change
// to the driver. Those run wherever the driver runs: in its background (interrupt) context on
// core 0, or in a caller holding its lock. The `wifi` health check judges the counts.

#ifndef SOS_WIFISTAT_H
#define SOS_WIFISTAT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t timeout;           // error returns: -CYW43_ETIMEDOUT (an ioctl's reply, a credit)
    uint32_t io;                // -CYW43_EIO (F2 not ready, and the bus set-up at boot)
    uint32_t bus;               // warnings: a bus error, a corrupt frame header, no wake-up
    uint32_t tx;                // warnings: a frame the driver couldn't send
    uint32_t other;             // other error returns and other warnings
    uint32_t held_back;         // warning lines not logged (one per site every 10 s)
    int last_res;               // the last error return's code (0: none yet)
    const char *last_func;      // its function
    uint32_t last_at_s;         // its uptime
} wifistat_t;

// A copy of the counts.
void wifistat_get(wifistat_t *out);

#ifdef SOS_TEST_COMMANDS
// `health fault wifi timeout|bus <n>`: count n errors as the driver's hooks would (a timeout's
// error return, or a bus warning, logged as the driver's would be).
void wifistat_inject(bool timeout, unsigned n);
#endif

#endif
