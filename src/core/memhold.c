// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Test-only heap holder (see memhold.h). Compiled only with -DSOS_TEST_COMMANDS=ON.

#ifndef SOS_TEST_COMMANDS
#error "memhold.c is a test tool and belongs only in test builds (-DSOS_TEST_COMMANDS=ON)"
#endif

#include "core/memhold.h"

#include <malloc.h>
#include <stdlib.h>

#include "core/heap.h"

static void *blocks[MEMHOLD_MAX_BLOCKS];
static size_t sizes[MEMHOLD_MAX_BLOCKS];
static int count;

static bool keep_block(void *p, size_t n) {
    if (!p)
        return false;
    if (count == MEMHOLD_MAX_BLOCKS) {
        free(p);
        return false;
    }
    blocks[count] = p;
    sizes[count] = n;
    count++;
    return true;
}

bool memhold_block(size_t n) {
    if (count == MEMHOLD_MAX_BLOCKS || n == 0)
        return false;
    return keep_block(malloc(n), n);
}

// The largest block up to `limit` that malloc gives now.
static size_t largest_upto(size_t limit) {
    size_t lo = 0, hi = limit + 1;          // lo fits, hi doesn't
    while (hi - lo > 16) {
        size_t mid = lo + (hi - lo) / 2;
        void *p = malloc(mid);
        if (p) {
            free(p);
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

size_t memhold_largest(void) {
    return largest_upto(heap_info().total);
}

size_t memhold_leave(size_t keep) {
    size_t taken = 0;
    while (count < MEMHOLD_MAX_BLOCKS) {
        size_t free_now = heap_free();
        if (free_now <= keep + 16)
            break;
        size_t n = largest_upto(free_now - keep);
        if (n < 16 || !keep_block(malloc(n), n))
            break;
        taken += n;
    }
    return taken;
}

int memhold_holes(int want, size_t size) {
    // Allocate hole, separator, hole, separator...: from the top of malloc's arena they lie
    // in that order. Then fill everything else and free the holes; each is bordered by held
    // memory, so they can't merge.
    void *holes[32];
    if (want > 32)
        want = 32;
    int made = 0;
    while (made < want && count < MEMHOLD_MAX_BLOCKS - 1) {
        void *h = malloc(size);
        if (!h)
            break;
        if (!keep_block(malloc(64), 64)) {
            free(h);
            break;
        }
        holes[made++] = h;
    }
    memhold_leave(0);
    for (int i = 0; i < made; i++)
        free(holes[i]);
    return made;
}

size_t memhold_release(void) {
    size_t freed = 0;
    while (count > 0) {
        count--;
        free(blocks[count]);
        freed += sizes[count];
        blocks[count] = NULL;
    }
    return freed;
}

size_t memhold_bytes(void) {
    size_t n = 0;
    for (int i = 0; i < count; i++)
        n += sizes[i];
    return n;
}

int memhold_count(void) {
    return count;
}
