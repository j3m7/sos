// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

// The `card` and `sd-bus` health checks (core/health_board.c lists them): files/card_check.c.

#ifndef SOS_CARD_CHECK_H
#define SOS_CARD_CHECK_H

#include <stdbool.h>

#include "core/health.h"

health_step_t card_check_step(void **state, bool cancel, health_result_t *out);
health_step_t sdbus_check_step(void **state, bool cancel, health_result_t *out);

#ifdef SOS_TEST_COMMANDS
// `health state-dump` (test builds): the lines the two checks read.
void card_check_dump(health_print_fn print, void *arg);
void sdbus_check_dump(health_print_fn print, void *arg);
#endif

#endif
