// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Wall-clock time. The RTC is the source of truth: at boot the system clock (time(),
// gettimeofday()) is set from it. The network time comes from NTS (authenticated NTP, src/time/nts.c):
// when the RTC has no valid time (retried with backoff until it works), on `time sync`, and every
// ntp_sync_hours if that setting is on (default off). A successful sync sets the system clock and
// the RTC.
//
// There is no automatic fallback to plain SNTP: it's unauthenticated. `time sync sntp` uses it
// once on request, and accepts a reply only if it's after the firmware build time.

#ifndef SOS_TIMESYNC_H
#define SOS_TIMESYNC_H

#include <stdbool.h>

#include "shell/shell.h"

// Set up the RTC and set the system clock from it. Call early in main(), before the network.
void timesync_boot(void);

// The network is up: start an NTS sync if the clock isn't set or a periodic sync is due (and no
// retry delay is pending). Cheap; call from the main loop while the network is up.
void timesync_network_up(void);

// `time sync`: an NTS sync now, even if the clock is set (taking over a periodic one in
// progress). Returns false if another sync is running.
bool timesync_request_nts(void);

// `time sync sntp`: one unauthenticated SNTP sync now. Returns false if a sync is running.
bool timesync_request_sntp(void);

// True once the system clock holds a real time (RTC, NTS or SNTP), so dates can be checked.
bool timesync_clock_set(void);

// Apply a sync result, if one arrived; stop a periodic sync that was turned off. Call from the
// main loop.
void timesync_poll(void);

// The `time` command output.
void timesync_status(shell_t *sh);

#endif
