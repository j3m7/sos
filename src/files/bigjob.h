// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// One archive job at a time, across all shells (docs/design/archives.md §5.2): fetch, and later
// gunzip, zcat, gzip and tar, each need tens of KB of heap for as long as they run, so while one
// runs the others are refused rather than two of them starving the heap together. Pure C (no
// SDK), host tested; main loop only.
//
//     const char *other = bigjob_try_begin("fetch");
//     if (other) {
//         shell_printf(sh, BIGJOB_BUSY_FMT, other);
//         return;
//     }
//     ...                                  // and bigjob_end() when the job ends, however it ends

#ifndef SOS_BIGJOB_H
#define SOS_BIGJOB_H

#include <stdbool.h>

// The message for a refused command, with the running one's name.
#define BIGJOB_BUSY_FMT "busy: %s is running in another shell\n"

// Start the job `name` (a string that outlives the job: the command's name as a literal).
// Returns NULL if it may run, or the name of the job already running.
const char *bigjob_try_begin(const char *name);

// The job that bigjob_try_begin() admitted has ended. Harmless when none runs.
void bigjob_end(void);

// The job running, or NULL.
const char *bigjob_running(void);

#endif
