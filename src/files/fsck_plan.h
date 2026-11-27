// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// fsck_plan: the words of `sd check fix` (docs/design/sd-check.md §10.3): what the plan of
// fsck_repair.h will do, what it leaves alone, and what a repair did. Pure C, no I/O: sdcard.c
// prints and logs the lines, tests/test_fsck_plan.c checks them.
//
// What is said follows fsck_plan_writes()'s rules: lost clusters are freed unless part of the
// tree wasn't read (`unread`), chains are trimmed unless that or a cross-link stops them, FAT
// copies and FSInfo are always written when the plan has them.

#ifndef SOS_FSCK_PLAN_H
#define SOS_FSCK_PLAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "files/fsck_repair.h"

#define FSCK_PLAN_LINE_MAX 120  // longest line of the functions below, with its terminator

// The plan before the question, a line at a time through `line` (no newlines): "to repair:"
// and an indented line per kind written, then "left alone:" and a line per thing skipped, then,
// with `problems` (the check found problems), that the others are only reported. Nothing for a
// plan with nothing to say (no problems, nothing to repair or skip). `cluster_bytes` is the
// check's cluster size, for the sizes shown.
void fsck_plan_show(const fsck_plan_t *p, uint32_t cluster_bytes, bool problems,
                    void (*line)(void *ctx, const char *s), void *ctx);

// The plan on one line, for the log: "free 16 lost clusters, trim 2 chains (5 clusters), copy 3
// FAT sectors, FSInfo; skipped: trims (cross-links)", or "nothing".
void fsck_plan_brief(const fsck_plan_t *p, char *out, size_t n);

// What a finished repair did (fsck_repair_done()): "freed 21 clusters (672 KB), trimmed 2
// chains, copied 3 FAT sectors, FSInfo's free count set to unknown; 9 sectors written", or
// "nothing written".
void fsck_done_line(const fsck_plan_t *d, uint32_t cluster_bytes, uint32_t writes, char *out,
                    size_t n);

// "512 bytes", "32 KB", "1.5 MB": whole units, or one decimal (as fsck.c's sizes). `out`
// holds 40 bytes for any size.
void fsck_plan_size(char *out, size_t n, uint64_t bytes);

#endif
