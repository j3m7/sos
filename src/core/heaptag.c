// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Heap use by feature: see heaptag.h.
//
// The counters are atomic because a tag can be used from both cores at once (Lua's state
// allocates on core 1 while core 0 holds the script's source under the same tag).

#include "core/heaptag.h"

#include <malloc.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static struct {
    atomic_size_t used, peak;
} tags[HEAP_TAGS];

static const char *const names[HEAP_TAGS] = {
    [HEAP_NET] = "ssh/tls",
    [HEAP_LUA] = "lua",
    [HEAP_EDIT] = "editor",
    [HEAP_FILES] = "files",
    [HEAP_HTTP] = "http",
    [HEAP_TIME] = "time",
    [HEAP_PING] = "ping",
};

static void grew(heap_tag_t tag, size_t n) {
    size_t now = atomic_fetch_add(&tags[tag].used, n) + n;
    size_t peak = atomic_load(&tags[tag].peak);
    while (now > peak && !atomic_compare_exchange_weak(&tags[tag].peak, &peak, now))
        ;
}

static void shrank(heap_tag_t tag, size_t n) {
    atomic_fetch_sub(&tags[tag].used, n);
}

void *heap_alloc(heap_tag_t tag, size_t n) {
    void *p = malloc(n);
    if (p)
        grew(tag, malloc_usable_size(p));
    return p;
}

void *heap_zalloc(heap_tag_t tag, size_t n) {
    void *p = calloc(1, n);
    if (p)
        grew(tag, malloc_usable_size(p));
    return p;
}

void heap_release(heap_tag_t tag, void *p) {
    if (!p)
        return;
    shrank(tag, malloc_usable_size(p));
    free(p);
}

void *heap_realloc(heap_tag_t tag, void *p, size_t n) {
    if (n == 0) {
        heap_release(tag, p);
        return NULL;
    }
    size_t old = p ? malloc_usable_size(p) : 0;
    void *q = realloc(p, n);
    if (!q)
        return NULL;                        // p is still allocated, still counted
    size_t now = malloc_usable_size(q);
    if (now > old)
        grew(tag, now - old);
    else
        shrank(tag, old - now);
    return q;
}

char *heap_strdup(heap_tag_t tag, const char *s) {
    size_t n = strlen(s) + 1;
    char *p = heap_alloc(tag, n);
    if (p)
        memcpy(p, s, n);
    return p;
}

const char *heap_tag_name(heap_tag_t tag) {
    return names[tag];
}

size_t heap_tag_used(heap_tag_t tag) {
    return atomic_load(&tags[tag].used);
}

size_t heap_tag_peak(heap_tag_t tag) {
    return atomic_load(&tags[tag].peak);
}
