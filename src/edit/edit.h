// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_EDIT_H
#define SOS_EDIT_H

#include "shell/shell.h"

// `edit <file>`: the full-screen text editor, on the shell's channel (console or SSH). Runs as a
// raw-mode shell job (shell_run_raw) until Ctrl-Q.
void edit_command(shell_t *sh, char *args);

#endif
