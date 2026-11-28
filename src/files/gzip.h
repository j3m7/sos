// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// gunzip and zcat: gzip files on the SD card, decompressed as shell jobs (files/gz.h does the
// decoding). docs/design/archives.md §5.

#ifndef SOS_GZIP_H
#define SOS_GZIP_H

#include "shell/shell.h"

// The commands, for shell_execute() and Tab (a table as shell.h describes).
extern const shell_cmd_t gzip_commands[];
void gzip_help(shell_t *sh);

#endif
