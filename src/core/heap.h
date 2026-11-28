// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_HEAP_H
#define SOS_HEAP_H

#include <stddef.h>
#include <stdint.h>

// The heap, shared by both cores: its figures (for `mem` and for deciding whether there's room
// for something big, such as a TLS connection), read safely while the other core allocates.
typedef struct {
    size_t total;       // the whole heap
    size_t used;        // allocated now
    size_t arena;       // taken from the heap by malloc so far (the high-water mark)
} heap_info_t;

heap_info_t heap_info(void);

// Bytes not allocated (total - used), inside malloc's arena or beyond it. Fragmentation can
// make the largest single block smaller.
size_t heap_free(void);

// The allocator's totals and the heap's end, taken together under its lock, for the `heap-sum`
// health check: mallinfo()'s bytes in use, free and the arena, and sbrk(0). Allocates nothing.
typedef struct {
    size_t used, free, arena;
    uintptr_t end;
} heap_snapshot_t;
void heap_snapshot(heap_snapshot_t *out);

#endif
