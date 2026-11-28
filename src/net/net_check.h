// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

// The `wifi` and `net-pools` health checks (core/health_board.c lists them): net/net_check.c.

#ifndef SOS_NET_CHECK_H
#define SOS_NET_CHECK_H

#include <stdbool.h>

#include "core/health.h"

health_step_t wifi_check_step(void **state, bool cancel, health_result_t *out);
health_step_t pools_check_step(void **state, bool cancel, health_result_t *out);

#ifdef SOS_TEST_COMMANDS
// Test builds: `health fault wifi timeout|bus <n>` counts n driver errors (false: bad words), and
// `health state-dump`'s lines for the two checks.
bool wifi_check_fault(const char *kind, const char *count);
void wifi_check_dump(health_print_fn print, void *arg);
void pools_check_dump(health_print_fn print, void *arg);
#endif

#endif
