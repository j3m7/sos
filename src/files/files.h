// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// File commands on the SD card (ls, cd, pwd, cat, cp, mv, rm, mkdir, echo). Long ones run as
// shell jobs, a bounded amount of work per main-loop pass.

#ifndef SOS_FILES_H
#define SOS_FILES_H

#include <stdbool.h>

#include "shell/shell.h"

// Run `cmd` if it's a file command; false if it isn't one.
bool files_command(shell_t *sh, const char *cmd, char *args);
void files_help(shell_t *sh);

// A file command or an SFTP transfer holds open files (formatting or unmounting would break it).
bool files_busy(void);
void files_hold(void);          // one more holder of open files
void files_release(void);

// The single place deciding whether a path on the card may be changed: called with the
// absolute path before every write, create, delete, rename (both names), mkdir and attribute
// change, by the shell file commands (`sh`: the session's shell) and SFTP (`sh` NULL). /certs
// (path_is_protected()) may only be changed from an admin shell (the console).
bool files_write_allowed(const shell_t *sh, const char *path);

// A short description of a FatFs result (FRESULT), e.g. "no such file".
const char *files_error(int fr);

#endif
