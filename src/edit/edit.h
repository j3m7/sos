// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_EDIT_H
#define SOS_EDIT_H

#include "shell/shell.h"

#include <stddef.h>

// `edit <file>`: the full-screen text editor, on the shell's channel (console or SSH). Runs as a
// raw-mode shell job (shell_run_raw) until Ctrl-Q.
void edit_command(shell_t *sh, char *args);

// `more <file>`: the pager on a file of the card, of any size (view.c). A raw-mode shell job
// until q.
void view_command(shell_t *sh, char *args);
// The pager on `len` bytes of text in RAM (copied), shown under `name`: for any command with
// more text to show than a screen (the manual pages, once there are some).
void edit_view(shell_t *sh, const char *name, const char *text, size_t len);

#endif
