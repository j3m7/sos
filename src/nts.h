// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// NTS (RFC 8915) client. Key establishment runs NTS-KE over TLS (src/tls.c) and keeps the keys
// and cookies in RAM; a query sends an NTS-protected NTP request (src/nts_ntp.c) using one
// cookie and authenticates the reply, which brings replacement cookies. Queries only measure
// and report the clock offset for now: setting the clock from NTS is the next step (issue #2).

#ifndef SOS_NTS_H
#define SOS_NTS_H

#include <stdbool.h>

#include "shell.h"

#define NTS_DEFAULT_SERVER "time.cloudflare.com"

// Start a key exchange with host (NULL = NTS_DEFAULT_SERVER). False if one, or a query, is
// running. Discards the previous keys and cookies.
bool nts_ke_start(const char *host);

// Start an NTS-protected NTP query with the current keys. NULL if started, otherwise why not.
const char *nts_query_start(void);

// Advance a running key exchange or query. Call from the main loop.
void nts_poll(void);

// The `nts` command output: key exchange, cookies, last query and measurements.
void nts_status(shell_t *sh);

#endif
