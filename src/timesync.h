// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Wall-clock time. The RTC is the source of truth: at boot the system clock (time(),
// gettimeofday()) is set from it. SNTP is used only when the RTC has no valid time, or when
// asked to with timesync_request_ntp() (the `time sync` command); a successful reply sets the
// system clock and the RTC, then SNTP stops.
//
// SNTP is unauthenticated, so a reply is only accepted if it is after the firmware build time.

#ifndef SOS_TIMESYNC_H
#define SOS_TIMESYNC_H

#include <stdbool.h>

#include "shell.h"

// Set up the RTC and set the system clock from it. Call early in main(), before the network.
void timesync_boot(void);

// The network is up: start SNTP if the clock isn't set and it isn't running. Cheap; call from
// the main loop while the network is up.
void timesync_network_up(void);

// Start an SNTP sync now, even if the clock is set. Returns false if one is already running.
bool timesync_request_ntp(void);

// Apply an SNTP result, if one arrived. Call from the main loop.
void timesync_poll(void);

// The `time` command output.
void timesync_status(shell_t *sh);

#endif
