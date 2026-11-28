// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

// The `tls` command (shell/shell.c lists it).

#ifndef SOS_TLS_CMD_H
#define SOS_TLS_CMD_H

#include "shell/shell.h"

void tls_command(shell_t *sh, char *args);      // `tls roots ...`
extern const shell_word_t tls_words[];          // what Tab offers after `tls`

#endif
