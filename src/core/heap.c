// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Heap figures, and newlib's malloc lock.
//
// The SDK's pico_malloc wraps malloc, calloc, realloc and free in its own mutex, so the two cores
// never allocate at once. But mallinfo() isn't wrapped: it walks malloc's free lists under
// newlib's __malloc_lock(), which by default does nothing. Reading the figures on core 0 while
// a Lua script on core 1 is inside realloc could then follow a pointer that has just become a
// script's data. So __malloc_lock()/__malloc_unlock() take a recursive mutex here (recursive
// because newlib's realloc calls malloc with the lock held); newlib calls them in malloc, free,
// realloc and mallinfo. Inside the SDK's wrappers it's the second lock taken, always in that
// order, and mallinfo takes only this one, so they can't deadlock. Nothing allocates from an
// interrupt: lwIP uses its own pools, the SSH accept callback leaves its allocations to the main
// loop, the TLS callbacks only fill a buffer allocated beforehand, and log_msg() formats with
// the SDK's printf, which doesn't allocate. If something ever did, it couldn't deadlock here
// either: the mutex's owner is the core, so an interrupt on the core holding it passes through
// (unprotected, as before this lock), and one on the other core waits only for that
// allocation to finish.

#include "core/heap.h"

#include <malloc.h>
#include <reent.h>
#include <unistd.h>

#include "pico/mutex.h"

auto_init_recursive_mutex(heap_lock);

void __malloc_lock(struct _reent *r) {
    (void)r;
    recursive_mutex_enter_blocking(&heap_lock);
}

void __malloc_unlock(struct _reent *r) {
    (void)r;
    recursive_mutex_exit(&heap_lock);
}

heap_info_t heap_info(void) {
    extern char __end__, __HeapLimit;
    struct mallinfo mi = mallinfo();
    heap_info_t h = {
        .total = (size_t)((uintptr_t)&__HeapLimit - (uintptr_t)&__end__),
        .used = mi.uordblks,
        .arena = mi.arena,
    };
    return h;
}

void heap_snapshot(heap_snapshot_t *out) {
    recursive_mutex_enter_blocking(&heap_lock);     // mallinfo() takes it again: recursive
    struct mallinfo mi = mallinfo();
    void *end = sbrk(0);
    recursive_mutex_exit(&heap_lock);
    out->used = mi.uordblks;
    out->free = mi.fordblks;
    out->arena = mi.arena;
    out->end = (uintptr_t)end;
}

size_t heap_free(void) {
    heap_info_t h = heap_info();
    return h.total > h.used ? h.total - h.used : 0;
}
