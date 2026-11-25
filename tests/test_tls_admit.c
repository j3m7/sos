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
        CHECK(tls_admit(u, u, TLS_USER, PLENTY) == NULL);
    const char *why = tls_admit(TLS_MAX_USER_CONNS, TLS_MAX_USER_CONNS, TLS_USER, PLENTY);
    CHECK(why != NULL && strstr(why, "HTTPS requests") != NULL);
    // ...but the system still gets in.
    CHECK(tls_admit(TLS_MAX_USER_CONNS, TLS_MAX_USER_CONNS, TLS_SYSTEM, PLENTY) == NULL);
}

TEST(system_connection_doesnt_count_against_users) {
    // NTS running: users still get their full share.
    CHECK(tls_admit(1, 0, TLS_USER, PLENTY) == NULL);
    CHECK(tls_admit(2, 1, TLS_USER, PLENTY) == NULL);
    CHECK(tls_admit(3, 2, TLS_USER, PLENTY) != NULL);
}

TEST(total_limit) {
    const char *why = tls_admit(TLS_MAX_CONNS, 0, TLS_SYSTEM, PLENTY);
    CHECK(why != NULL && strstr(why, "too many TLS connections") != NULL);
    CHECK(tls_admit(TLS_MAX_CONNS, 1, TLS_USER, PLENTY) != NULL);
    CHECK(tls_admit(TLS_MAX_CONNS - 1, 0, TLS_SYSTEM, PLENTY) == NULL);
}

TEST(low_heap_is_refused) {
    const char *why = tls_admit(0, 0, TLS_USER, TLS_HEAP_NEEDED - 1);
    CHECK(why != NULL && strstr(why, "memory") != NULL);
    CHECK(tls_admit(0, 0, TLS_SYSTEM, TLS_HEAP_NEEDED - 1) != NULL);   // for the system too
    CHECK(tls_admit(0, 0, TLS_USER, TLS_HEAP_NEEDED) == NULL);
    CHECK(tls_admit(0, 0, TLS_SYSTEM, 0) != NULL);
}

TEST_MAIN(
    RUN(limits_are_consistent);
    RUN(user_requests_leave_a_slot_for_the_system);
    RUN(system_connection_doesnt_count_against_users);
    RUN(total_limit);
    RUN(low_heap_is_refused);
)
