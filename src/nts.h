// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// NTS (RFC 8915) client, part 1: key establishment. Runs NTS-KE over TLS (src/tls.c) and keeps
// the negotiated keys and cookies in RAM for the NTS-protected NTP client (not written yet).
// See issue #2.

#ifndef SOS_NTS_H
#define SOS_NTS_H

#include <stdbool.h>

#include "shell.h"

#define NTS_DEFAULT_SERVER "time.cloudflare.com"

// Start a key exchange with host (NULL = NTS_DEFAULT_SERVER). False if one is running.
bool nts_ke_start(const char *host);

// Advance a running key exchange. Call from the main loop.
void nts_poll(void);

// The `nts` command output: last key exchange, its result and measurements.
void nts_status(shell_t *sh);

#endif
