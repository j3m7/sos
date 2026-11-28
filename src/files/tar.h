// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// tar tf and tar xf: tar archives on the SD card (plain or gzipped) listed and extracted as shell
// jobs (files/tar_parse.h reads them). docs/design/archives.md §6.

#ifndef SOS_TAR_H
#define SOS_TAR_H

#include "shell/shell.h"

// The command, for shell_execute() and Tab (a table as shell.h describes).
extern const shell_cmd_t tar_commands[];
void tar_help(shell_t *sh);

#endif
