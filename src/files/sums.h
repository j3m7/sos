// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// sha1sum, sha256sum and sha512sum: a file's hash, and checking files against a list
// (docs/design/archives.md §4). Shell jobs, like cat.

#ifndef SOS_SUMS_H
#define SOS_SUMS_H

#include "shell/shell.h"

// The commands, for shell_execute() and Tab (a table as shell.h describes).
extern const shell_cmd_t sums_commands[];

#endif
