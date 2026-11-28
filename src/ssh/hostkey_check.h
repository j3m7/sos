// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

// The `hostkey` health check (core/health_board.c lists it): ssh/hostkey_check.c.

#ifndef SOS_HOSTKEY_CHECK_H
#define SOS_HOSTKEY_CHECK_H

#include <stdbool.h>

#include "core/health.h"

health_step_t hostkey_check_step(void **state, bool cancel, health_result_t *out);

#ifdef SOS_TEST_COMMANDS
// `health state-dump` (test builds): the line the check reads.
void hostkey_check_dump(health_print_fn print, void *arg);
#endif

#endif
