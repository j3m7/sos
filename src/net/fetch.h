// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// `fetch [-f] [-k] <url> [<file>]`: download a URL to a file on the card, over the HTTPS client
// (net/http.h), following redirects (docs/design/archives.md §3). The body goes to <file>.part
// and is renamed to <file> only once it's complete, so a cut download never looks finished. The
// summary gives the SHA-256 of what was written, hashed as it was written.

#ifndef SOS_FETCH_H
#define SOS_FETCH_H

#include "shell/shell.h"

// The command (a `runv` handler: shell.h).
void fetch_command(shell_t *sh, int argc, char **argv);

#endif
