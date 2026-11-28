// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Phase 6's health checks (docs/design/health.md §9.9): `wifi` and `net-pools` are net/'s
// (net/net_check.c), `sd-bus` the card's (files/card_check.c), since
// docs/design/build-features.md §4.3. What's left here is the test commands' dispatch, which
// prints the state dump's lines in the order they always had.

#include "core/countcheck_board.h"

#include "files/card_check.h"
#if SOS_HAVE_NET
#include "net/net_check.h"
#endif

#ifdef SOS_TEST_COMMANDS
bool countcheck_board_wifi_fault(const char *kind, const char *count) {
#if SOS_HAVE_NET
    return wifi_check_fault(kind, count);
#else
    (void)kind;
    (void)count;
    return false;                       // no Wi-Fi in this image
#endif
}

void countcheck_board_dump(health_print_fn print, void *arg) {
#if SOS_HAVE_NET
    wifi_check_dump(print, arg);
#endif
    sdbus_check_dump(print, arg);
#if SOS_HAVE_NET
    pools_check_dump(print, arg);
#endif
}
#endif
