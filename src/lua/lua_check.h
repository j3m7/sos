// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

// The `core1` health check (core/health_board.c lists it): lua/lua_check.c.

#ifndef SOS_LUA_CHECK_H
#define SOS_LUA_CHECK_H

#include <stdbool.h>

#include "core/health.h"

health_step_t lua_check_core1_step(void **state, bool cancel, health_result_t *out);

#ifdef SOS_TEST_COMMANDS
// `health state-dump` (test builds): the line the check reads.
void lua_check_dump(health_print_fn print, void *arg);
#endif

#endif
