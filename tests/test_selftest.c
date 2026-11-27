// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/selftest.c, the `crypto` and `crypto-ssh` health checks' known-answer
// tests, against wolfCrypt built for the host with the firmware's options for every primitive
// tested (tests/wolfcrypt_host/user_settings.h): every vector passes, and a fault made in each
// test in turn (a flipped bit in what it computed, or a refusal test fed the unchanged input) is
// caught by that test. The heap is heaptag.c's, so a leak shows in its counters.

#include <string.h>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/wc_port.h>

#include "core/heaptag.h"
#include "core/selftest.h"
#include "test.h"

// Run every test; the index of the first that failed (count if none) and its phrase.
static unsigned run_all(const char **why) {
    selftest_t *st = selftest_open();
    *why = st ? NULL : "no memory";
    unsigned i = 0;
    for (; st && i < selftest_count(); i++) {
        *why = selftest_run(st, i);
        if (*why)
            break;
    }
    selftest_close(st);
    return i;
}

TEST(all_pass) {
    const char *why = "";
    CHECK_EQ(run_all(&why), selftest_count());
    if (why)
        fprintf(stderr, "  failed: %s\n", why);
    CHECK(why == NULL);
    CHECK_EQ(selftest_count(), 13);
    CHECK_EQ(heap_tag_used(HEAP_NET), 0);           // the contexts are given back
}

// The firmware has an RNG, so wolfCrypt blinds Curve25519, and so does this build: the X25519
// test has to give the key an RNG, as the board's first run showed (#242).
TEST(same_curve25519_options_as_the_firmware) {
#ifndef WOLFSSL_CURVE25519_BLINDING
    CHECK(!"WOLFSSL_CURVE25519_BLINDING is off: not the firmware's Curve25519");
#endif
#ifdef CURVED25519_128BIT
    CHECK(!"the 128-bit field code: not the firmware's");
#endif
#ifdef CURVE25519_SMALL
    CHECK(!"CURVE25519_SMALL: not the firmware's");
#endif
    CHECK(1);
}

TEST(each_fault_is_caught_where_it_is) {
    unsigned n = selftest_count();
    const char *seen[16] = {0};
    static char copies[16][64];
    for (unsigned i = 0; i < n; i++) {
        selftest_fault((int)i);
        const char *why = NULL;
        CHECK_EQ(run_all(&why), i);                 // that test, not an earlier one
        CHECK(why != NULL);
        if (why) {
            snprintf(copies[i], sizeof(copies[i]), "%s", why);
            seen[i] = copies[i];
            fprintf(stderr, "  fault in %s: %s\n", selftest_info(i)->name, why);
        }
    }
    selftest_fault(-1);
    const char *why = "";
    CHECK_EQ(run_all(&why), n);                     // and cleared again
    CHECK_EQ(heap_tag_used(HEAP_NET), 0);
    // Some phrases that a person reads on the board.
    CHECK(seen[5] && !strcmp(seen[5], "aes-gcm accepted a changed ciphertext"));
    CHECK(seen[9] && !strcmp(seen[9], "ed25519 gave a wrong signature"));
    CHECK(seen[11] && !strcmp(seen[11], "ed25519 verify accepted a changed message"));
    CHECK(seen[12] && !strcmp(seen[12], "x25519 gave a wrong shared secret"));
    for (unsigned i = 0; i < n; i++)
        CHECK(seen[i] && strlen(seen[i]) < 64);    // each fits a check's text
}

// The boot subset: everything making keys and sealing at boot rely on, nothing slow.
TEST(boot_subset) {
    unsigned boot = 0;
    for (unsigned i = 0; i < selftest_count(); i++) {
        const selftest_info_t *t = selftest_info(i);
        bool slow = t->group == SELFTEST_ED_SIGN || t->group == SELFTEST_ED_VERIFY ||
                    t->group == SELFTEST_X25519;
        CHECK_EQ(t->boot, !slow);
        boot += t->boot;
    }
    CHECK_EQ(boot, 9);
    CHECK(selftest_info(selftest_count()) == NULL);
    selftest_t *st = selftest_open();
    CHECK(!strcmp(selftest_run(st, 99), "no such test"));
    selftest_close(st);
}

TEST(text_with_times) {
    char out[64];
    uint32_t us[SELFTEST_GROUPS] = {400, 1240, 300, 19300, 0, 0, 0};
    unsigned runs[SELFTEST_GROUPS] = {3, 3, 2, 1, 0, 0, 0};
    selftest_text(out, sizeof(out), 9, 21240, us, runs);
    CHECK(!strcmp(out, "9 known answers in 21 ms: sha 0.4 gcm 1.2 xts 0.3 ed-pub 19"));
    uint32_t us2[SELFTEST_GROUPS] = {0, 0, 0, 0, 19000, 112000, 52000};
    unsigned runs2[SELFTEST_GROUPS] = {0, 0, 0, 0, 1, 2, 1};
    selftest_text(out, sizeof(out), 4, 183000, us2, runs2);
    CHECK(!strcmp(out, "4 known answers in 183 ms: ed-sign 19 ed-verify 112 x25519 52"));
    // Whole groups only, when it doesn't fit.
    char small[40];
    selftest_text(small, sizeof(small), 4, 183000, us2, runs2);
    CHECK(!strcmp(small, "4 known answers in 183 ms: ed-sign 19"));
}

TEST_MAIN(
    wolfCrypt_Init();
    RUN(same_curve25519_options_as_the_firmware);
    RUN(all_pass);
    RUN(each_fault_is_caught_where_it_is);
    RUN(boot_subset);
    RUN(text_with_times);
    wolfCrypt_Cleanup();
)
