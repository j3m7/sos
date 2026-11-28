// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

// The `time` and `nts` commands (shell/shell.c lists them).

#ifndef SOS_TIME_CMD_H
#define SOS_TIME_CMD_H

#include "shell/shell.h"

void time_command(shell_t *sh, char *args);     // `time ...`
void nts_command(shell_t *sh, char *args);      // `nts ...`
extern const shell_word_t time_words[];         // what Tab offers after `time`
extern const shell_word_t nts_words[];          // and after `nts`

// The NTS servers the board knows, for Tab: the build's default and the one set with
// `time server` (also offered by `ping`).
void time_nts_servers(shell_value_fn fn, void *arg);

#endif
