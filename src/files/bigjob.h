// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The big file jobs, one at a time across all shells (docs/design/archives.md §5.2): gunzip,
// zcat, and later gzip, tar and fetch each need ~50 KB of heap or more for as long as they run,
// so while one runs another is refused, as http.c refuses a third request. Pure C.

#ifndef SOS_BIGJOB_H
#define SOS_BIGJOB_H

#include <stdbool.h>

// Take the slot for the command `name` (a string that lives as long as the job: its name).
// False if another job holds it, with *running set to that job's name: the caller prints
// "busy: <running> is running in another shell".
bool bigjob_begin(const char *name, const char **running);

// Give the slot back (the job that took it has ended, whatever the outcome).
void bigjob_end(void);

#endif
