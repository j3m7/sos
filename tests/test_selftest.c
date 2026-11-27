// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/selftest.c, the `crypto` health check's known-answer tests, against
// wolfCrypt built for the host: every vector passes, and a fault made in each test in turn (a
// flipped bit in what it computed, or a refusal test fed the unchanged input) is caught by that
// test and no earlier one. The heap is heaptag.c's, so a leak shows in its counters.

#include <string.h>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/wc_port.h>

#include "core/heaptag.h"
#include "core/selftest.h"
#include "test.h"

TEST(all_pass) {
    unsigned ran = 99;
    CHECK(selftest_crypto(&ran) == NULL);
    CHECK_EQ(ran, selftest_crypto_count());
    CHECK_EQ(selftest_crypto_count(), 13);
    CHECK_EQ(heap_tag_used(HEAP_NET), 0);           // the contexts are given back
}

TEST(each_fault_is_caught_where_it_is) {
    unsigned n = selftest_crypto_count();
    const char *seen[16] = {0};
    for (unsigned i = 0; i < n; i++) {
        selftest_crypto_fault((int)i);
        unsigned ran = 0;
        const char *why = selftest_crypto(&ran);
        CHECK(why != NULL);
        CHECK_EQ(ran, i + 1);                       // that test, not an earlier one
        seen[i] = why;
        if (why)
            fprintf(stderr, "  fault in test %u: %s\n", i, why);
    }
    selftest_crypto_fault(-1);
    unsigned ran = 0;
    CHECK(selftest_crypto(&ran) == NULL);           // and cleared again
    CHECK_EQ(heap_tag_used(HEAP_NET), 0);
    // Some phrases that a person reads on the board.
    CHECK(seen[5] && !strcmp(seen[5], "aes-gcm accepted a changed ciphertext"));
    CHECK(seen[9] && !strcmp(seen[9], "ed25519 gave a wrong signature"));
    CHECK(seen[11] && !strcmp(seen[11], "ed25519 verify accepted a changed message"));
    CHECK(seen[12] && !strcmp(seen[12], "x25519 gave a wrong shared secret"));
    // Each fits a check's text.
    for (unsigned i = 0; i < n; i++)
        CHECK(seen[i] && strlen(seen[i]) < 64);
}

TEST_MAIN(
    wolfCrypt_Init();
    RUN(all_pass);
    RUN(each_fault_is_caught_where_it_is);
    wolfCrypt_Cleanup();
)
