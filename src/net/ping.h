// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// `ping <host> [<count>]`: ICMP echo requests to a name or an IPv4 address, one a second, as a
// shell job (Ctrl-C stops it). Each shell can run one at the same time as the others. See
// docs/commands.md for what it prints, and src/net/README.md for how it works.

#ifndef SOS_PING_H
#define SOS_PING_H

#include "shell/shell.h"

void ping_command(shell_t *sh, char *args);

#endif
