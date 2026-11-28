// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The pure half of phase 6's health checks, the counters (core/health.h, docs/design/health.md
// §9.9): `wifi`, `sd-bus` and `net-pools`, with the windows they judge over and the classifiers
// the counting hooks use. countcheck_board.c reads the counters (kept by net/wifistat.c,
// net/wifi.c, files/sdcard.c and lwIP) and hands them here; tests/test_countcheck.c runs these
// with made-up inputs.
//
// None of these checks holds a gate, and none touches the Wi-Fi chip or the card: they judge
// what the code that saw the events counted. Every figure is provisional until a 24-hour
// baseline on dev-1 (§9.9).

#ifndef SOS_COUNTCHECK_H
#define SOS_COUNTCHECK_H

#include <stdbool.h>
#include <stdint.h>

#include "core/health.h"

// --- windows: how many of a counter's events fell in the last hour -------------------------------

#define COUNTCHECK_VALUES   3           // counters per window
#define COUNTCHECK_SAMPLES  8
#define COUNTCHECK_GAP_S    600         // a sample at most this often
#define COUNTCHECK_WINDOW_S 3600        // the window: from the newest sample at least this old

typedef struct {
    uint32_t at_s;                      // uptime
    uint32_t v[COUNTCHECK_VALUES];      // the counters then (since boot)
} countcheck_sample_t;

typedef struct {
    countcheck_sample_t s[COUNTCHECK_SAMPLES];
    unsigned n;                         // samples held
    unsigned next;                      // where the next one goes
} countcheck_window_t;                  // zero it at boot

// The counters' growth over the window ending now: from the newest sample at least
// COUNTCHECK_WINDOW_S old, or from boot (when every counter was 0) if there is none. *span_s says
// how long the window is. Then stores now as a sample, unless the newest is under
// COUNTCHECK_GAP_S old. With COUNTCHECK_SAMPLES samples at least COUNTCHECK_GAP_S apart, a full
// ring always holds one old enough, so the window is between one hour and one hour plus a gap.
void countcheck_window(countcheck_window_t *w, uint32_t now_s,
                       const uint32_t now_v[COUNTCHECK_VALUES],
                       uint32_t delta[COUNTCHECK_VALUES], uint32_t *span_s);

// --- wifi ----------------------------------------------------------------------------------------

#define COUNTCHECK_WIFI_WARN_ERRORS 3       // judged errors in the window
#define COUNTCHECK_WIFI_FAIL_ERRORS 30
#define COUNTCHECK_WIFI_WARN_DROPS  4       // link drops in COUNTCHECK_DROP_WINDOW_S
#define COUNTCHECK_DROP_WINDOW_S    86400
#define COUNTCHECK_WIFI_DOWN_S      1800    // set up but not connected this long

typedef enum {
    COUNTCHECK_WIFI_UNSET,              // no network saved
    COUNTCHECK_WIFI_STOPPED,            // saved, but Wi-Fi isn't running
    COUNTCHECK_WIFI_DOWN,               // trying to connect
    COUNTCHECK_WIFI_UP,
} countcheck_wifi_state_t;

typedef struct {
    countcheck_wifi_state_t state;
    uint32_t down_s;                    // DOWN: since the link was last up (or Wi-Fi started)
    uint32_t timeouts, io, bus;         // the judged errors, in the window
    uint32_t span_s;                    // the window's length
    unsigned drops;                     // link drops not asked for, in the last 24 h
    uint32_t last_drop_ago_s;           // the newest of them
    const char *fw;                     // the chip's version ("7.95.49"), or NULL if not read
    const char *driver;                 // the driver's ("1.1.0")
} countcheck_wifi_t;

// SKIP while not set up or stopped. FAIL at COUNTCHECK_WIFI_FAIL_ERRORS judged errors in the
// window; WARN at COUNTCHECK_WIFI_WARN_ERRORS, at COUNTCHECK_WIFI_WARN_DROPS drops in 24 h, after
// COUNTCHECK_WIFI_DOWN_S without a connection, or without the firmware version; PASS otherwise.
void countcheck_judge_wifi(const countcheck_wifi_t *in, health_result_t *out);

// How many of the n drop times (uptimes) lie within `within_s` before now, and how long ago the
// newest of them was (0 if none).
unsigned countcheck_drops_within(const uint32_t *times, unsigned n, uint32_t now_s,
                                 uint32_t within_s, uint32_t *newest_ago_s);

// "7.95.49" from the chip's `ver` answer ("wl0: Nov 29 2021 ... version 7.95.49 (2271bb6 CY) ...")
// into out; false (out empty) if it has no "version " followed by a digit.
bool countcheck_fw_version(const char *ver, char *out, unsigned cap);

// The kind a CYW43_WARN line counts as, by the function it comes from (countcheck_names.h).
typedef enum {
    COUNTCHECK_WIFI_NONE,               // logged only: counted by its error return
    COUNTCHECK_WIFI_BUS,
    COUNTCHECK_WIFI_TX,
    COUNTCHECK_WIFI_OTHER,
} countcheck_wifi_kind_t;
countcheck_wifi_kind_t countcheck_wifi_warn_kind(const char *func);

// --- sd-bus --------------------------------------------------------------------------------------

#define COUNTCHECK_SD_FAIL_FAILED   10      // failed reads and writes since the init
#define COUNTCHECK_SD_RETRY_MIN     3       // retried errors before a rate counts
#define COUNTCHECK_SD_RETRY_BYTES   (10u * 1024 * 1024)     // one retried error per this many bytes
#define COUNTCHECK_SD_SPEC_KHZ      25000   // the SD spec's maximum in SPI mode

typedef struct {
    bool ever_ok;                       // an initialisation has worked since boot
    bool last_init_failed;
    uint32_t khz;                       // the SPI clock in use
    uint64_t bytes;                     // read and written since the last initialisation
    uint32_t failed_reads, failed_writes;   // after the driver's retries (level A)
    uint32_t crc, cmd, write;           // retried errors (level B)
    uint32_t timeouts;                  // level B
} countcheck_sd_t;

// The first that applies: SKIP no initialisation has worked since boot; FAIL
// COUNTCHECK_SD_FAIL_FAILED failed reads and writes; WARN a failed one; WARN the last
// initialisation failed; WARN a timeout; WARN a retried error above 25 MHz; WARN
// COUNTCHECK_SD_RETRY_MIN retried errors and one per COUNTCHECK_SD_RETRY_BYTES; PASS, naming a
// clock above 25 MHz.
void countcheck_judge_sd(const countcheck_sd_t *in, health_result_t *out);

// The kind a debug line (DBG_PRINTF, error_line false) or an error line (EMSG_PRINTF, true) of the
// SD card library's sd_card_spi.c counts as, by the function it comes from (countcheck_names.h).
// `initialising`: the driver's init is running (sd_cmd's lines then count nothing).
typedef enum {
    COUNTCHECK_SD_NONE,
    COUNTCHECK_SD_CRC,
    COUNTCHECK_SD_TIMEOUT,
    COUNTCHECK_SD_CMD,
    COUNTCHECK_SD_WRITE,
} countcheck_sd_kind_t;
countcheck_sd_kind_t countcheck_sd_kind(const char *func, bool initialising, bool error_line);

// --- net-pools -----------------------------------------------------------------------------------

typedef struct {
    uint32_t pbuf, seg, heap;           // failures in the window: packet pool, segments, lwIP heap
    uint32_t span_s;
    unsigned pool_max, pool_avail;      // the packet pool's most used, and its size
} countcheck_pools_t;

// WARN on any failure in the window (the packet pool's named first); PASS otherwise.
void countcheck_judge_pools(const countcheck_pools_t *in, health_result_t *out);

#endif
