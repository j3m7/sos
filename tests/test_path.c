// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/path.c: resolving and joining the file commands' paths.

#include "path.h"
#include "test.h"

static char out[64];

#define CHECK_RESOLVE(cwd, path, expected) do {                                             \
        CHECK(path_resolve((cwd), (path), out, sizeof(out)));                               \
        test_checks++;                                                                      \
        if (strcmp(out, (expected)) != 0) {                                                 \
            test_failures++;                                                                \
            fprintf(stderr, "%s:%d: resolve(\"%s\", \"%s\") = \"%s\", expected \"%s\"\n",    \
                    __FILE__, __LINE__, (cwd), (path), out, (expected));                    \
        }                                                                                   \
    } while (0)

TEST(relative_and_absolute) {
    CHECK_RESOLVE("/", "a", "/a");
    CHECK_RESOLVE("/docs", "a.txt", "/docs/a.txt");
    CHECK_RESOLVE("/docs", "/b", "/b");             // absolute ignores cwd
    CHECK_RESOLVE("/docs/x", "sub/f", "/docs/x/sub/f");
    CHECK_RESOLVE("/", "", "/");
    CHECK_RESOLVE("/docs", "", "/docs");            // empty: the directory itself
}

TEST(dots_and_slashes) {
    CHECK_RESOLVE("/docs", ".", "/docs");
    CHECK_RESOLVE("/docs", "..", "/");
    CHECK_RESOLVE("/a/b/c", "../..", "/a");
    CHECK_RESOLVE("/a/b", "../c/./d/", "/a/c/d");
    CHECK_RESOLVE("/", "..", "/");                  // never above the root
    CHECK_RESOLVE("/a", "../../../x", "/x");
    CHECK_RESOLVE("/", "//a///b//", "/a/b");
    CHECK_RESOLVE("/a", "/..", "/");
    CHECK_RESOLVE("/", "...", "/...");              // a name, not a parent
    CHECK_RESOLVE("/", ".hidden", "/.hidden");
    CHECK_RESOLVE("/ab", "../ab/../cd", "/cd");
}

TEST(too_long_is_refused) {
    char small[8];
    CHECK(path_resolve("/", "abcdef", small, sizeof(small)));     // "/abcdef" + NUL = 8
    CHECK(!strcmp(small, "/abcdef"));
    CHECK(!path_resolve("/", "abcdefg", small, sizeof(small)));
    CHECK(!path_resolve("/abc", "defg", small, sizeof(small)));
    CHECK(path_resolve("/abc", "../defghi", small, sizeof(small)));   // fits after ".."
    CHECK(!path_resolve("/", "a", small, 1));
    char two[2];
    CHECK(path_resolve("/", "..", two, sizeof(two)));
    CHECK(!strcmp(two, "/"));
}

TEST(basename) {
    CHECK(!strcmp(path_basename("/a/b.txt"), "b.txt"));
    CHECK(!strcmp(path_basename("/b"), "b"));
    CHECK(!strcmp(path_basename("/"), ""));
}

TEST(join) {
    CHECK(path_join("/", "a", out, sizeof(out)) && !strcmp(out, "/a"));
    CHECK(path_join("/d", "a", out, sizeof(out)) && !strcmp(out, "/d/a"));
    char small[5];
    CHECK(!path_join("/d", "ab", small, sizeof(small)));          // "/d/ab" + NUL = 6
    CHECK(path_join("/d", "a", small, sizeof(small)) && !strcmp(small, "/d/a"));
    CHECK(path_join("/", "abc", small, sizeof(small)) && !strcmp(small, "/abc"));
}

TEST_MAIN(
    RUN(relative_and_absolute);
    RUN(dots_and_slashes);
    RUN(too_long_is_refused);
    RUN(basename);
    RUN(join);
)
