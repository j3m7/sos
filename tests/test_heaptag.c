// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Host tests for src/core/heaptag.c: heap use by feature.

#include "core/heaptag.h"

#include "test.h"

TEST(alloc_and_release) {
    size_t peak0 = heap_tag_peak(HEAP_FILES);
    CHECK_EQ(heap_tag_used(HEAP_FILES), 0);
    char *a = heap_alloc(HEAP_FILES, 1000);
    CHECK(a != NULL);
    size_t one = heap_tag_used(HEAP_FILES);
    CHECK(one >= 1000);                             // the usable size, at least what was asked
    char *b = heap_zalloc(HEAP_FILES, 500);
    CHECK(b != NULL);
    for (int i = 0; i < 500; i++)
        CHECK(b[i] == 0);
    size_t two = heap_tag_used(HEAP_FILES);
    CHECK(two >= one + 500);
    CHECK_EQ(heap_tag_peak(HEAP_FILES), two > peak0 ? two : peak0);
    heap_release(HEAP_FILES, a);
    CHECK_EQ(heap_tag_used(HEAP_FILES), two - one);
    heap_release(HEAP_FILES, b);
    CHECK_EQ(heap_tag_used(HEAP_FILES), 0);
    CHECK_EQ(heap_tag_peak(HEAP_FILES), two);       // the peak stays
    heap_release(HEAP_FILES, NULL);                 // like free(NULL)
    CHECK_EQ(heap_tag_used(HEAP_FILES), 0);
}

TEST(tags_are_separate) {
    void *a = heap_alloc(HEAP_EDIT, 64);
    void *b = heap_alloc(HEAP_HTTP, 4096);
    CHECK(heap_tag_used(HEAP_EDIT) >= 64 && heap_tag_used(HEAP_EDIT) < 4096);
    CHECK(heap_tag_used(HEAP_HTTP) >= 4096);
    CHECK_EQ(heap_tag_used(HEAP_TIME), 0);
    heap_release(HEAP_EDIT, a);
    CHECK_EQ(heap_tag_used(HEAP_EDIT), 0);
    CHECK(heap_tag_used(HEAP_HTTP) >= 4096);
    heap_release(HEAP_HTTP, b);
    CHECK_EQ(heap_tag_used(HEAP_HTTP), 0);
}

TEST(realloc_follows_the_size) {
    char *p = heap_realloc(HEAP_LUA, NULL, 100);    // like malloc
    CHECK(p != NULL);
    memset(p, 'x', 100);
    size_t small = heap_tag_used(HEAP_LUA);
    CHECK(small >= 100);
    p = heap_realloc(HEAP_LUA, p, 20000);
    CHECK(p != NULL && p[0] == 'x' && p[99] == 'x');
    size_t big = heap_tag_used(HEAP_LUA);
    CHECK(big >= 20000);
    CHECK_EQ(heap_tag_peak(HEAP_LUA), big);
    p = heap_realloc(HEAP_LUA, p, 50);
    CHECK(p != NULL && p[0] == 'x' && p[49] == 'x');
    CHECK(heap_tag_used(HEAP_LUA) >= 50 && heap_tag_used(HEAP_LUA) <= big);
    CHECK_EQ(heap_tag_peak(HEAP_LUA), big);
    CHECK(heap_realloc(HEAP_LUA, p, 0) == NULL);    // releases
    CHECK_EQ(heap_tag_used(HEAP_LUA), 0);
    CHECK(heap_realloc(HEAP_LUA, NULL, 0) == NULL);
    CHECK_EQ(heap_tag_used(HEAP_LUA), 0);
}

TEST(strdup_counts) {
    char *s = heap_strdup(HEAP_NET, "print('hello')");
    CHECK(s != NULL && !strcmp(s, "print('hello')"));
    CHECK(heap_tag_used(HEAP_NET) >= 15);
    heap_release(HEAP_NET, s);
    CHECK_EQ(heap_tag_used(HEAP_NET), 0);
}

TEST(names) {
    for (int t = 0; t < HEAP_TAGS; t++)
        CHECK(heap_tag_name((heap_tag_t)t) != NULL && strlen(heap_tag_name((heap_tag_t)t)) <= 8);
    CHECK(!strcmp(heap_tag_name(HEAP_NET), "ssh/tls"));
}

TEST_MAIN(
    RUN(alloc_and_release);
    RUN(tags_are_separate);
    RUN(realloc_follows_the_size);
    RUN(strdup_counts);
    RUN(names);
)
