// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/tls/tls_admit.c: which new TLS connections may open.

#include "tls/tls_admit.h"
#include "test.h"

#define PLENTY (TLS_HEAP_NEEDED * 4)

TEST(limits_are_consistent) {
    CHECK(TLS_MAX_USER_CONNS >= 1);
    CHECK(TLS_MAX_USER_CONNS < TLS_MAX_CONNS);      // always one left for the system
}

TEST(user_requests_leave_a_slot_for_the_system) {
    for (unsigned u = 0; u < TLS_MAX_USER_CONNS; u++)
        CHECK(tls_admit(u, u, 0, TLS_USER, false, PLENTY) == NULL);
    const unsigned max = TLS_MAX_USER_CONNS;
    const char *why = tls_admit(max, max, 0, TLS_USER, false, PLENTY);
    CHECK(why != NULL && strstr(why, "HTTP requests") != NULL);
    // ...but the system still gets in.
    CHECK(tls_admit(max, max, 0, TLS_SYSTEM, false, PLENTY) == NULL);
}

TEST(system_connection_doesnt_count_against_users) {
    // NTS running: users still get their full share.
    CHECK(tls_admit(1, 0, 0, TLS_USER, false, PLENTY) == NULL);
    CHECK(tls_admit(2, 1, 0, TLS_USER, false, PLENTY) == NULL);
    CHECK(tls_admit(3, 2, 0, TLS_USER, false, PLENTY) != NULL);
}

TEST(total_limit) {
    const char *why = tls_admit(TLS_MAX_CONNS, 0, 0, TLS_SYSTEM, false, PLENTY);
    CHECK(why != NULL && strstr(why, "too many TLS connections") != NULL);
    CHECK(tls_admit(TLS_MAX_CONNS, 1, 0, TLS_USER, false, PLENTY) != NULL);
    CHECK(tls_admit(TLS_MAX_CONNS - 1, 0, 0, TLS_SYSTEM, false, PLENTY) == NULL);
}

TEST(low_heap_is_refused) {
    const char *why = tls_admit(0, 0, 0, TLS_USER, false, TLS_HEAP_NEEDED - 1);
    CHECK(why != NULL && strstr(why, "memory") != NULL);
    CHECK(tls_admit(0, 0, 0, TLS_SYSTEM, false, TLS_HEAP_NEEDED - 1) != NULL);  // the system too
    CHECK(tls_admit(0, 0, 0, TLS_USER, false, TLS_HEAP_NEEDED) == NULL);
    CHECK(tls_admit(0, 0, 0, TLS_SYSTEM, false, 0) != NULL);
}

TEST(connections_still_opening_count_against_the_heap) {
    // One handshake under way: room for it and the new one is needed.
    CHECK(tls_admit(1, 1, 1, TLS_USER, false, TLS_HEAP_NEEDED * 2) == NULL);
    CHECK(tls_admit(1, 1, 1, TLS_USER, false, TLS_HEAP_NEEDED * 2 - 1) != NULL);
    CHECK(tls_admit(2, 1, 2, TLS_SYSTEM, false, TLS_HEAP_NEEDED * 3 - 1) != NULL);
    CHECK(tls_admit(2, 1, 2, TLS_SYSTEM, false, TLS_HEAP_NEEDED * 3) == NULL);
    // Finished handshakes have already taken their memory: only the new one counts.
    CHECK(tls_admit(2, 2, 0, TLS_SYSTEM, false, TLS_HEAP_NEEDED) == NULL);
}

TEST(refusal_names_the_limit) {
    char expect[16];
    snprintf(expect, sizeof(expect), "(%d at most", TLS_MAX_USER_CONNS);
    const unsigned max = TLS_MAX_USER_CONNS;
    const char *why = tls_admit(max, max, 0, TLS_USER, false, PLENTY);
    CHECK(why && strstr(why, expect) != NULL);
}

TEST(plain_connections_share_the_slots) {
    // Plain http (fetch) counts as a user request: two of any kind, then refused, and the
    // system's slot is still kept.
    CHECK(tls_admit(1, 1, 0, TLS_USER, true, PLENTY) == NULL);
    CHECK(tls_admit(2, 2, 0, TLS_USER, true, PLENTY) != NULL);
    CHECK(tls_admit(2, 2, 0, TLS_USER, false, PLENTY) != NULL);
    CHECK(tls_admit(2, 2, 0, TLS_SYSTEM, false, PLENTY) == NULL);
    CHECK(tls_admit(TLS_MAX_CONNS, 1, 0, TLS_USER, true, PLENTY) != NULL);
}

TEST(plain_connections_need_less_heap) {
    CHECK(TLS_PLAIN_HEAP_NEEDED < TLS_HEAP_NEEDED);
    CHECK(tls_admit(0, 0, 0, TLS_USER, true, TLS_PLAIN_HEAP_NEEDED) == NULL);
    const char *why = tls_admit(0, 0, 0, TLS_USER, true, TLS_PLAIN_HEAP_NEEDED - 1);
    CHECK(why && strstr(why, "memory") != NULL);
    // A TLS handshake under way still needs its share.
    CHECK(tls_admit(1, 0, 1, TLS_USER, true, TLS_HEAP_NEEDED + TLS_PLAIN_HEAP_NEEDED) == NULL);
    CHECK(tls_admit(1, 0, 1, TLS_USER, true, TLS_HEAP_NEEDED + TLS_PLAIN_HEAP_NEEDED - 1) !=
          NULL);
}

TEST_MAIN(
    RUN(limits_are_consistent);
    RUN(user_requests_leave_a_slot_for_the_system);
    RUN(system_connection_doesnt_count_against_users);
    RUN(total_limit);
    RUN(low_heap_is_refused);
    RUN(connections_still_opening_count_against_the_heap);
    RUN(refusal_names_the_limit);
    RUN(plain_connections_share_the_slots);
    RUN(plain_connections_need_less_heap);
)
