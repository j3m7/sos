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
#include <stdint.h>

#include "shell/shell.h"
#include "time/timeconv.h"

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

// Put the saved RTC calibration (settings()->rtc_offset) into the RTC if it isn't there already.
// Call after settings_init(), and whenever the setting changes. False if the RTC didn't take it
// (or isn't there).
bool timesync_apply_cal(void);
// "off", "slowed by 91.1 ppm" or "sped up by 13.0 ppm": what the saved calibration does to the RTC.
void timesync_cal_str(char *buf, unsigned len);

// Apply a sync result, if one arrived; stop a periodic sync that was turned off. Call from the
// main loop.
void timesync_poll(void);

// The `time` command output.
// The state `time` prints, as values (for Lua's sos.clock()). Strings point at static or settings
// storage and stay valid until the next sync starts or the configuration changes.
#define TIMESYNC_NEVER UINT32_MAX
typedef struct {
    const char *source;         // where the clock's time came from: "rtc", "nts", "sntp"; NULL: not set
    const char *syncing;        // the server of a sync running now, NULL if none
    bool syncing_sntp;          // ...and that sync is plain SNTP
    uint32_t synced_ago_s;      // since the last accepted sync, TIMESYNC_NEVER: none since boot
    bool step_set;              // that sync set a clock that had no time (step_ms then means nothing)
    int64_t step_ms;            // how far it moved the clock
    int64_t delay_ms;           // its round trip (NTS)
    uint32_t failed_ago_s;      // since the last failed sync, TIMESYNC_NEVER: none since the last success
    const char *failed_why;
    uint32_t retry_in_s;        // until the next try after that failure, 0 if none is planned
    const char *server;         // the NTS server used
    bool server_default;        // ...the build's default, not one set with `time server`
    unsigned auto_hours;        // periodic sync interval, 0: off
} timesync_info_t;
void timesync_info(timesync_info_t *info);

// Drift (docs/design/health.md §9.5): what the last accepted sync measured of the RTC's rate
// against its baseline (the time it was set to at a sync before, kept in the history sector across
// restarts) and of the crystal's (the step, when the sync before was in this uptime). Before any
// sync since boot: whether a baseline is waiting. Read by the `rtc-drift` health check and `time`.
void timesync_drift(timeconv_drift_t *out);

void timesync_status(shell_t *sh);

#endif
