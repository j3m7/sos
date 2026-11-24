// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Wall-clock time. The RTC is the source of truth: at boot the system clock (time(),
// gettimeofday()) is set from it. SNTP is used when the RTC has no valid time, when asked to
// with timesync_request_ntp() (the `time sync` command), and every ntp_sync_hours if that
// setting is on (default off). A successful reply sets the system clock and the RTC, then SNTP
// stops.
//
// SNTP is unauthenticated, so a reply is only accepted if it is after the firmware build time,
// and a periodic sync refuses to move a set clock by more than a few minutes.

#ifndef SOS_TIMESYNC_H
#define SOS_TIMESYNC_H

#include <stdbool.h>

#include "shell.h"

// Set up the RTC and set the system clock from it. Call early in main(), before the network.
void timesync_boot(void);

// The network is up: start SNTP if the clock isn't set or a periodic sync is due. Cheap; call
// from the main loop while the network is up.
void timesync_network_up(void);

// Start an SNTP sync now, even if the clock is set (taking over a periodic one in progress).
// Returns false if a non-periodic one is already running.
bool timesync_request_ntp(void);

// True once the system clock holds a real time (from the RTC or NTP), so dates can be checked.
bool timesync_clock_set(void);

// Apply an SNTP result, if one arrived; stop a periodic sync that was turned off. Call from the
// main loop.
void timesync_poll(void);

// The `time` command output.
void timesync_status(shell_t *sh);

#endif
