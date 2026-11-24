// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Minimal test harness for host-side unit tests: no dependencies, one executable per suite.
//
//   TEST(name) { CHECK(cond); CHECK_EQ(actual, expected); }
//   TEST_MAIN(RUN(name); RUN(other);)

#ifndef SOS_TEST_H
#define SOS_TEST_H

#include <stdio.h>
#include <string.h>

static int test_checks, test_failures;
static const char *test_current;

#define TEST(name) static void name(void)

#define CHECK(cond) do {                                                                    \
        test_checks++;                                                                      \
        if (!(cond)) {                                                                      \
            test_failures++;                                                                \
            fprintf(stderr, "%s:%d: %s: CHECK(%s) failed\n",                                \
                    __FILE__, __LINE__, test_current, #cond);                               \
        }                                                                                   \
    } while (0)

#define CHECK_EQ(actual, expected) do {                                                     \
        long long a_ = (long long)(actual), e_ = (long long)(expected);                     \
        test_checks++;                                                                      \
        if (a_ != e_) {                                                                     \
            test_failures++;                                                                \
            fprintf(stderr, "%s:%d: %s: %s == %lld, expected %s == %lld\n",                 \
                    __FILE__, __LINE__, test_current, #actual, a_, #expected, e_);          \
        }                                                                                   \
    } while (0)

#define CHECK_MEM(actual, expected, len) do {                                               \
        test_checks++;                                                                      \
        if (memcmp((actual), (expected), (len)) != 0) {                                     \
            test_failures++;                                                                \
            fprintf(stderr, "%s:%d: %s: memory of %s differs from %s (%zu bytes)\n",        \
                    __FILE__, __LINE__, test_current, #actual, #expected, (size_t)(len));   \
        }                                                                                   \
    } while (0)

#define RUN(name) do { test_current = #name; name(); } while (0)

#define TEST_MAIN(...)                                                                      \
    int main(void) {                                                                        \
        __VA_ARGS__                                                                         \
        printf("%d checks, %d failed\n", test_checks, test_failures);                       \
        return test_failures ? 1 : 0;                                                       \
    }

#endif
