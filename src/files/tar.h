// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// tar tf, tar xf, tar cf and tar czf: tar archives on the SD card (plain or gzipped) listed,
// extracted (tar.c, reading with files/tar_parse.h) and made (tar_create.c, writing with
// files/tar_write.h) as shell jobs. docs/design/archives.md §6.

#ifndef SOS_TAR_H
#define SOS_TAR_H

#include <stdbool.h>

#include "shell/shell.h"

// The command, for shell_execute() and Tab (a table as shell.h describes).
extern const shell_cmd_t tar_commands[];
void tar_help(shell_t *sh);

// tar cf / czf, once tar.c has read the words: the archive `archive` made from the n `paths`
// (relative to `dir`, or the working directory when NULL), gzipped if `gz`, replacing an
// existing one only if `force`, each name printed as it is added if `verbose`. Runs as a job;
// the strings are copied.
void tar_create(shell_t *sh, const char *archive, const char *dir, bool gz, bool force,
                bool verbose, int n, char **paths);

#endif
