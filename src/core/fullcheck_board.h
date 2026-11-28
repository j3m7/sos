// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Phase 5's health checks on the board, the full tier (docs/design/health.md §9.7): the steps
// health_board.c lists, the copy of the image's hash taken at boot, and what `health state-dump`
// and `health fault` add in test builds. The judging is in fullcheck.h.

#ifndef SOS_FULLCHECK_BOARD_H
#define SOS_FULLCHECK_BOARD_H

#include <stdbool.h>

#include "core/health.h"

// In health_boot(), before the boot tier: copy the image's hash items (its HASH_VALUE among
// them) as the boot ROM has just verified them, for `image` to compare the flash with later.
void fullcheck_boot(void);

health_step_t fullcheck_ram_step(void **state, bool cancel, health_result_t *out);
health_step_t fullcheck_image_step(void **state, bool cancel, health_result_t *out);
health_step_t fullcheck_sd_step(void **state, bool cancel, health_result_t *out);

#ifdef SOS_TEST_COMMANDS
// `health fault ram once|twice` and `health fault image`: a fault for the next run of that
// check only. `once` flips a bit of the first heap block under test between a write and its read
// in the first test (a WARN), `twice` in its second test too (a FAIL); `image` flips one byte of
// what the next hash reads (a FAIL). Nothing in flash or in memory in use is touched. False if
// `what` names none of them.
bool fullcheck_board_arm_fault(const char *what, const char *how);

// `health state-dump`'s phase 5 lines: the RAM test's last mismatch, the hash items copied at
// boot and the load map.
void fullcheck_board_dump(health_print_fn print, void *arg);
#endif

#endif
