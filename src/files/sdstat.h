// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The SD card bus's counters, for the `sd-bus` health check (docs/design/health.md §9.9, §9.10).
// files/sdcard.c counts what passes its wrappers around the driver; this file also counts what
// the driver reports from inside itself: its debug lines (debug_message_printf(), defined here)
// and its error lines (sdstat_error_line(), from sdcard.c's error_message_printf()).
//
// THE RULE for everything the driver calls from its own paths: count and return. Those hooks run
// inside the driver's init and retry loops, on a card that may have just failed; they never
// format, never log, never call into sdcard.c, FatFs, the log files or the driver, and never
// wait. Logging there could reach the log files' writer, which mounts the card, which runs the
// driver's init again (seconds each) from inside its own error path. The finding is logged by
// the check's step, from the main loop. tests/CMakeLists.txt checks that sdstat.c references no
// function but countcheck_sd_kind() (the `sdstat-leaf` test).
//
// Pure C, host tested (tests/test_countcheck.c).

#ifndef SOS_SDSTAT_H
#define SOS_SDSTAT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t sectors_read, sectors_written;
    uint32_t failed_reads, failed_writes;   // after the driver's retries
    uint32_t crc, cmd, write, timeout;      // retried errors, and timeouts (2 s each)
} sdcard_ctr_t;

typedef struct {
    sdcard_ctr_t since_init;            // since the last real initialisation (its own errors in)
    sdcard_ctr_t since_boot;
    uint32_t inits, init_fails;         // real initialisations since boot, and those that failed
    bool ever_ok;                       // one has worked since boot
    bool last_init_failed;
    uint32_t init_at_s;                 // the last one's uptime
    int last_error;                     // the driver's code for the last failed read or write
    uint32_t khz_set;                   // `sd speed`, as set
    uint32_t khz;                       // in use: the SPI block's, once the driver has set it up
} sdcard_counters_t;

// The counters (the khz fields are filled in by sdcard_counters(), sdcard.c).
extern sdcard_counters_t sdstat;
// The driver's init is running (init_layer() in sdcard.c sets it around the driver's init).
extern bool sdstat_initialising;

// One error line (EMSG_PRINTF) of the driver, from the function `func`: counted by function.
void sdstat_error_line(const char *func);

// Add n to a counter since the init and since boot.
#define SDSTAT_COUNT(field, n) (sdstat.since_init.field += (n), sdstat.since_boot.field += (n))

#endif
