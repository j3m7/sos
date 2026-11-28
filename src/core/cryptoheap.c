// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

// See cryptoheap.h. Moved from tls/tls.c (tls_init()), docs/design/build-features.md §4.2.

#include "core/cryptoheap.h"

#include <malloc.h>
#include <stddef.h>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/memory.h>

#include "core/heap.h"
#include "core/heaptag.h"

// Kept apart from HEAP_NET's counter, which these hooks also feed: that one includes the
// application's own SSH, TLS and certificate blocks, and a connection's peak is wolfSSL's alone.
static int64_t heap_now;
static cryptoheap_watch_fn watcher;

static void note_peak(void) {
    if (watcher)
        watcher(heap_now);
}

static void *count_malloc(size_t n) {
    void *p = heap_alloc(HEAP_NET, n);
    if (p) {
        heap_now += (int64_t)malloc_usable_size(p);
        note_peak();
    }
    return p;
}

static void count_free(void *p) {
    if (p)
        heap_now -= (int64_t)malloc_usable_size(p);
    heap_release(HEAP_NET, p);
}

static void *count_realloc(void *p, size_t n) {
    size_t old = p ? malloc_usable_size(p) : 0;
    void *q = heap_realloc(HEAP_NET, p, n);
    if (q || n == 0) {
        heap_now += (int64_t)(q ? malloc_usable_size(q) : 0) - (int64_t)old;
        note_peak();
    }
    return q;
}

void cryptoheap_init(void) {
    wolfSSL_SetAllocators(count_malloc, count_free, count_realloc);
}

int64_t cryptoheap_in_use(void) {
    return heap_now;
}

void cryptoheap_watch(cryptoheap_watch_fn fn) {
    watcher = fn;
}
