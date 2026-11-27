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
// The pager on a file of the card, shown under `name` (the status row; messages start with it
// too, so a command passes its own name, or "more" to show the path): `man` uses it.
void edit_view_file(shell_t *sh, const char *path, const char *name);
// The pager on `len` bytes of text in RAM (copied), shown under `name`: for any command with
// more text to show than a screen.
void edit_view(shell_t *sh, const char *name, const char *text, size_t len);
// The same on a HEAP_EDIT block the pager owns from here on (freed when it quits, or at once if
// it can't start): a command's captured output (`<command> | more`, shell_page()).
void edit_view_take(shell_t *sh, const char *name, char *text, size_t len);

#endif
