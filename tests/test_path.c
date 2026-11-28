// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/files/path.c: resolving and joining the file commands' paths.

#include "files/path.h"
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

TEST(protected_dir) {
    CHECK(path_is_protected("/certs"));
    CHECK(path_is_protected("/certs/roots.pem"));
    CHECK(path_is_protected("/certs/sub/dir/x"));
    CHECK(!path_is_protected("/"));
    CHECK(!path_is_protected("/docs/certs"));       // only the top-level directory
    CHECK(!path_is_protected("/certsx"));
    CHECK(!path_is_protected("/cert"));
    CHECK(!path_is_protected("/certs-old/x"));
    CHECK(!path_is_protected("/x.certs"));
}

TEST(protected_dir_fatfs_aliases) {
    // Names FatFs resolves to the same directory.
    CHECK(path_is_protected("/CERTS/x"));           // case-insensitive
    CHECK(path_is_protected("/Certs"));
    CHECK(path_is_protected("/certs./x"));          // trailing dots and spaces are stripped
    CHECK(path_is_protected("/certs ./x"));
    CHECK(path_is_protected("/certs. . ."));
    CHECK(path_is_protected("\\certs\\x"));         // '\' is a separator too
    CHECK(path_is_protected("/certs\\x"));
    CHECK(path_is_protected("//certs//x"));
    CHECK(path_is_protected("/./certs/./x"));
    CHECK(path_is_protected("/docs/../certs/x"));   // ".." is followed
    CHECK(path_is_protected("/docs\\..\\certs"));
    CHECK(path_is_protected("/a/b/../../certs"));
    // Names FatFs treats as different, so not protected.
    CHECK(!path_is_protected("/ certs/x"));         // leading spaces are kept
    CHECK(!path_is_protected("/certs/../x"));       // leaves the directory
    CHECK(!path_is_protected("/.. /certs"));        // ".. " is a (bad) name, not the parent...
    CHECK(path_is_protected("/../certs"));          // ...but ".." is, clamped at the root
    CHECK(!path_is_protected("/cert\xc5\xbf"));     // U+017F isn't case-folded to 's' by FatFs
}

TEST(log_dir_is_protected) {
    CHECK(path_is_protected("/var/log"));
    CHECK(path_is_protected("/var/log/"));
    CHECK(path_is_protected("/var/log/auth.log"));
    CHECK(path_is_protected("/var/log/auth.log.1"));
    CHECK(path_is_protected("/var/log/.rotated"));
    CHECK(path_is_protected("/var"));               // renaming /var would take /var/log along
    CHECK(path_is_protected("/var/"));
    CHECK(!path_is_protected("/var/tmp"));          // the rest of /var isn't
    CHECK(!path_is_protected("/var/tmp/x"));
    CHECK(!path_is_protected("/var/logs"));
    CHECK(!path_is_protected("/var/lo"));
    CHECK(!path_is_protected("/docs/var/log/x"));   // only at the top
    CHECK(!path_is_protected("/variant"));
    CHECK(!path_is_protected("/log"));
}

TEST(log_dir_fatfs_aliases) {
    CHECK(path_is_protected("/VAR/LOG/auth.log"));
    CHECK(path_is_protected("/Var/Log"));
    CHECK(path_is_protected("/var./log./x"));
    CHECK(path_is_protected("/var /log /x"));
    CHECK(path_is_protected("\\var\\log\\auth.log"));
    CHECK(path_is_protected("/var//log"));
    CHECK(path_is_protected("/var/./log/x"));
    CHECK(path_is_protected("/var/tmp/../log/x"));
    CHECK(path_is_protected("/docs/../var/log"));
    CHECK(path_is_protected("/var/log/../log/x"));
    CHECK(!path_is_protected("/var/log/../tmp"));   // leaves it
    CHECK(path_is_protected("/VAR."));
    CHECK(path_is_protected("/var/log\x01"));      // fails closed like /certs
}

TEST(protected_dir_fails_closed) {
    CHECK(path_is_protected("0:/certs"));           // drive prefix
    CHECK(path_is_protected("/0:/docs"));           // any ':' at all
    char lng[600];
    memset(lng, 'a', sizeof(lng) - 1);
    lng[0] = '/';
    lng[sizeof(lng) - 1] = '\0';
    CHECK(path_is_protected(lng));                  // too long to check
    // FatFs ends a path at a control character: "/certs\x01" is "/certs" to it.
    CHECK(path_is_protected("/certs\x01"));
    CHECK(path_is_protected("/certs\x01zz/y"));
    CHECK(path_is_protected("/certs\tz"));
    CHECK(path_is_protected("certs\x1f"));
    CHECK(path_is_protected("/docs/a\nb"));         // anywhere in the path
    CHECK(path_is_protected("/docs\x7f"));
}

TEST(name_check) {
    static const char *const ok[] = {
        "pkg-1.2.tar.gz", "SHA256SUMS", "a", ".hidden", "my file.txt", "caf\xc3\xa9",
        "\xe6\x97\xa5\xe6\x9c\xac", "\xf0\x9f\x98\x80", "a+b=c,d;e[f]", "x..y",
    };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        const char *why = path_name_check(ok[i]);
        CHECK(why == NULL);
        if (why)
            fprintf(stderr, "  \"%s\": %s\n", ok[i], why);
    }
    static const char *const bad[] = {
        "", ".", "..", "a/b", "a\\b", "a:b", "a*b", "a?b", "a\"b", "a<b", "a>b", "a|b",
        "trailing.", "trailing ", "a\x01", "\x1b[2J", "a\x7f",
        "\xc2\x9b" "x",                     // C1 CSI, UTF-8-encoded
        "\xff", "\xc3", "\xc3(", "\xc0\xaf", "\xe0\x80\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(path_name_check(bad[i]) != NULL);

    // 255 UTF-16 units at most: 255 ASCII, or 127 emoji (2 units each) plus one.
    static char name[1024];
    memset(name, 'n', 255);
    name[255] = '\0';
    CHECK(path_name_check(name) == NULL);
    name[255] = 'n';
    name[256] = '\0';
    CHECK(path_name_check(name) != NULL);
    char *p = name;
    for (int i = 0; i < 127; i++)
        p += sprintf(p, "\xf0\x9f\x98\x80");
    strcpy(p, "n");
    CHECK(path_name_check(name) == NULL);
    strcpy(p, "nn");
    CHECK(path_name_check(name) != NULL);
}

TEST_MAIN(
    RUN(relative_and_absolute);
    RUN(dots_and_slashes);
    RUN(too_long_is_refused);
    RUN(basename);
    RUN(join);
    RUN(protected_dir);
    RUN(log_dir_is_protected);
    RUN(log_dir_fatfs_aliases);
    RUN(protected_dir_fatfs_aliases);
    RUN(protected_dir_fails_closed);
    RUN(name_check);
)
