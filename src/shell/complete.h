// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_COMPLETE_H
#define SOS_COMPLETE_H

#include <stdbool.h>
#include <stddef.h>

#include "shell/shell.h"

// Tab completion: which words could finish the line being typed. Pure C over the command
// tables (shell.h), so it builds on the host for unit tests.

typedef void (*complete_fn)(const char *word, void *arg);

// The last word of `line` (`len` bytes, the cursor at its end; it may be empty) is the one Tab
// completes; it starts at *start. The words before it pick the level: the first is a command
// from `tables`, each later one a word that may follow the one before. Calls `fn` with every
// word of that level that starts with the last word, except SHELL_HIDDEN ones and, unless
// `admin`, SHELL_ADMIN ones, and returns how many. None when an earlier word isn't known (or
// is offered only to an admin channel and this isn't one), or takes no words after it. Words
// are separated by spaces; spaces before the command don't count.
size_t complete_words(const shell_cmd_t *const *tables, bool admin, const char *line, size_t len,
                      size_t *start, complete_fn fn, void *arg);

#endif
