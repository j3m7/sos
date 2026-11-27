// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// File commands on the SD card (ls, cd, pwd, cat, cp, mv, rm, mkdir, echo). Long ones run as
// shell jobs, a bounded amount of work per main-loop pass.

#ifndef SOS_FILES_H
#define SOS_FILES_H

#include <stdbool.h>
#include <stdint.h>

#include "ff.h"

#include "shell/shell.h"

// The file commands, for shell_execute() and Tab (a table as shell.h describes).
extern const shell_cmd_t files_commands[];
void files_help(shell_t *sh);

// `cat <file>` as a job, for commands that print a file as plain text when their output is piped
// (`more <file> | tail 5`). `arg` is the file as the user typed it (relative to the shell's
// directory); messages start with `who` (the calling command's name).
void files_cat(shell_t *sh, const char *who, const char *arg);

// A file command or an SFTP transfer holds open files (formatting or unmounting would break it).
bool files_busy(void);
void files_hold(void);          // one more holder of open files
void files_release(void);

// The single place deciding whether a path on the card may be changed: called with the
// absolute path before every write, create, delete, rename (both names), mkdir and attribute
// change, by the shell file commands (`sh`: the session's shell) and SFTP (`sh` NULL). /certs
// (path_is_protected()) may only be changed from an admin shell (the console).
bool files_write_allowed(const shell_t *sh, const char *path);

// FatFs writes a file's directory entry (its size and date) only at f_sync() or f_close(), so
// until then `ls` sees a new file at size 0 and date zero, and a reset loses what was written
// (#218). Whoever writes a file for a while (tee and `>`, cp, SFTP uploads) calls this after each
// write that wrote something, with `*synced_at` set to time_us_64() at the open: it syncs the file
// when `*synced_at` (the open, or the last sync) is a second old, and then sets it to now. When no
// sync is due it costs one clock read and a compare, so a fast writer is unaffected; a sync costs
// a directory-sector write and a partial data-sector rewrite, at most one a second per file.
// Returns f_sync()'s result, or FR_OK when no sync was due.
FRESULT files_sync_if_due(FIL *f, uint64_t *synced_at);

// A short description of a FatFs result (FRESULT), e.g. "no such file".
const char *files_error(int fr);

#endif
