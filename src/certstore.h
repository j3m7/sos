// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Extra TLS trust anchors from the SD card (issue #2): /certs/roots.pem, used only while its
// SHA-256 matches the hash approved on the console (`tls roots approve`, kept in the config).
// /certs itself can only be changed on the console (files_write_allowed()). If the file is
// missing, changed or unreadable, TLS uses the built-in roots alone (src/tls_roots.c).

#ifndef SOS_CERTSTORE_H
#define SOS_CERTSTORE_H

#include "shell.h"

#define CERTSTORE_FILE "/certs/roots.pem"
#define CERTSTORE_FILE_MAX (64 * 1024)

struct WOLFSSL_CTX;

// Add the approved SD roots to a TLS context. Returns how many were loaded: 0 when none are
// approved (the card isn't touched then) or the file doesn't match its approval. Logs changes
// of state (e.g. the file changed since approval) once, not per connection.
unsigned certstore_load(struct WOLFSSL_CTX *ctx);

// `tls roots [approve | forget]`. Approving and forgetting need the console.
void certstore_command(shell_t *sh, char *args);

#endif
