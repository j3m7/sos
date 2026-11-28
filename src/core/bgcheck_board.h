// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Phase 4's health checks on the board (docs/design/health.md §9.5): the steps health_board.c
// lists, and what `health state-dump` adds. The judging is in bgcheck.h.

#ifndef SOS_BGCHECK_BOARD_H
#define SOS_BGCHECK_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "core/health.h"

health_step_t bgcheck_resets_step(void **state, bool cancel, health_result_t *out);  // boot tier
health_step_t bgcheck_drift_step(void **state, bool cancel, health_result_t *out);
health_step_t bgcheck_heap_step(void **state, bool cancel, health_result_t *out);

// POWMAN's BOD register, for `supply` (bgcheck_judge_bod()).
uint32_t bgcheck_bod_register(void);

#ifdef SOS_TEST_COMMANDS
// `health state-dump`'s phase 4 lines: CHIP_RESET and the watchdog's REASON as read at boot, the
// BOD register, the history sector and its newest records, the drift record, the heap's totals
// and core 1's heartbeat.
void bgcheck_board_dump(health_print_fn print, void *arg);

// `health heap-walk`: the walk of newlib's chunks and free lists (core/heapwalk.h), under the
// allocator's lock for at most 8 ms, printed a line at a time. Changes nothing.
void bgcheck_board_heap_walk(health_print_fn print, void *arg);
#endif

#endif
