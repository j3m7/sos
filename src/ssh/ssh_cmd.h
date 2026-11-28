// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

// The `key` and `ssh` commands (shell/shell.c lists them).

#ifndef SOS_SSH_CMD_H
#define SOS_SSH_CMD_H

#include <stdbool.h>

#include "shell/shell.h"

void key_command(shell_t *sh, char *args);      // `key [list] | key add [line] | key del <n>`
void ssh_command(shell_t *sh, char *args);      // `ssh`: SSH server status
extern const shell_word_t key_words[];          // what Tab offers after `key`

// Parse and add one authorized_keys line (`key add`, `setup`). Returns true if the key was
// added.
bool key_add_line(shell_t *sh, const char *line);

#endif
