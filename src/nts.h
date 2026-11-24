// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// NTS (RFC 8915) client. Key establishment runs NTS-KE over TLS (src/tls.c) and keeps the keys
// and cookies in RAM; a query sends an NTS-protected NTP request (src/nts_ntp.c) using one
// cookie and authenticates the reply, which brings replacement cookies. A sync combines the two
// and picks the best of several queries; src/timesync.c sets the clock from it.

#ifndef SOS_NTS_H
#define SOS_NTS_H

#include <stdbool.h>

#include "shell.h"

#include <stdint.h>

// Default NTS-KE server (CMake SOS_NTS_SERVER); `time server` overrides it.
#define NTS_DEFAULT_SERVER SOS_NTS_SERVER

// Start a key exchange with host (NULL = NTS_DEFAULT_SERVER). False if one, or a query or
// sync, is running. Discards the previous keys and cookies.
bool nts_ke_start(const char *host);

// --- sync: what timesync uses ---------------------------------------------------------------

#define NTS_SYNC_SAMPLES 4      // queries per sync; the one with the smallest delay is used

typedef enum { NTS_SYNC_IDLE, NTS_SYNC_RUNNING, NTS_SYNC_OK, NTS_SYNC_FAILED } nts_sync_state_t;

// Get authenticated time from host: a key exchange if there are no keys or cookies for it,
// then up to NTS_SYNC_SAMPLES queries (another key exchange after an NTS NAK). False if busy.
bool nts_sync_start(const char *host);

// The outcome of the sync. NTS_SYNC_OK and NTS_SYNC_FAILED are returned once, then IDLE.
// On OK, *offset_us is server minus local clock and *delay_us the round trip of the sample
// used; on FAILED, *error says why.
nts_sync_state_t nts_sync_result(int64_t *offset_us, int64_t *delay_us, const char **error);

// Start an NTS-protected NTP query with the current keys. NULL if started, otherwise why not.
const char *nts_query_start(void);

// Advance a running key exchange or query. Call from the main loop.
void nts_poll(void);

// The `nts` command output: key exchange, cookies, last query and measurements.
void nts_status(shell_t *sh);

#endif
