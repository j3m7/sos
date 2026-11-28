// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Phase 6's health checks on the board, the counters (docs/design/health.md §9.9): the steps
// health_board.c lists, and what `health state-dump` and `health fault wifi` add in test builds.
// The judging is in countcheck.h.

#ifndef SOS_COUNTCHECK_BOARD_H
#define SOS_COUNTCHECK_BOARD_H

#include <stdbool.h>

#include "core/health.h"


#ifdef SOS_TEST_COMMANDS
// `health fault wifi timeout|bus <n>`: n driver errors counted as the driver's hooks count them
// (net/wifistat.h), for the next runs of `wifi` to judge. False if the words don't say that.
bool countcheck_board_wifi_fault(const char *kind, const char *count);

// `health state-dump`'s phase 6 lines: every counter, since boot and in its window.
void countcheck_board_dump(health_print_fn print, void *arg);
#endif

#endif
