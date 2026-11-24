// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// File commands on the SD card (ls, cd, pwd, cat, cp, mv, rm, mkdir, echo). Long ones run as
// shell jobs, a bounded amount of work per main-loop pass.

#ifndef SOS_FILES_H
#define SOS_FILES_H

#include <stdbool.h>

#include "shell.h"

// Run `cmd` if it's a file command; false if it isn't one.
bool files_command(shell_t *sh, const char *cmd, char *args);
void files_help(shell_t *sh);

// A file command is running and holds open files (formatting or unmounting would break it).
bool files_busy(void);

#endif
