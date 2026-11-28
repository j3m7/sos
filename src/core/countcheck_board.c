// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Phase 6's health checks (docs/design/health.md §9.9): `wifi` and `net-pools` are net/'s
// (net/net_check.c), `sd-bus` the card's (files/card_check.c), since
// docs/design/build-features.md §4.3. What's left here is the test commands' dispatch, which
// prints the state dump's lines in the order they always had.

#include "core/countcheck_board.h"

#include "files/card_check.h"
#include "net/net_check.h"

#ifdef SOS_TEST_COMMANDS
bool countcheck_board_wifi_fault(const char *kind, const char *count) {
    return wifi_check_fault(kind, count);
}

void countcheck_board_dump(health_print_fn print, void *arg) {
    wifi_check_dump(print, arg);
    sdbus_check_dump(print, arg);
    pools_check_dump(print, arg);
}
#endif
