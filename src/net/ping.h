// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// `ping <host> [<count>]`: ICMP echo requests to a name or an IPv4 address, one a second, as a
// shell job (Ctrl-C stops it). Each shell can run one at the same time as the others. See
// docs/commands.md for what it prints, and src/net/README.md for how it works.

#ifndef SOS_PING_H
#define SOS_PING_H

#include "net/ping_msg.h"
#include "shell/shell.h"

void ping_command(shell_t *sh, char *args);

// A ping without a shell (Lua's sos.ping): the same requests, one a second, nothing printed.
typedef struct ping_run ping_run_t;

// Start `count` requests to host (a name is looked up first). NULL and *err if it can't start
// (Wi-Fi down, too many pings at once, out of memory).
ping_run_t *ping_begin(const char *host, unsigned count, const char **err);

// Move it on, from the main loop: false once it has finished.
bool ping_advance(ping_run_t *r);

// Once finished: why it couldn't ping (the DNS lookup failed or timed out), or NULL.
const char *ping_failure(const ping_run_t *r);

// What came back so far, and the address pinged ("" until DNS answered).
const ping_stats_t *ping_run_stats(const ping_run_t *r);
void ping_run_address(const ping_run_t *r, char *out, size_t n);

// Free it, finished or not.
void ping_end(ping_run_t *r);

#endif
