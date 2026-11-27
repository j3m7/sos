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
#include "pico/time.h"           // time_us_64

static void *blocks[MEMHOLD_MAX_BLOCKS];
static size_t sizes[MEMHOLD_MAX_BLOCKS];
static int count;
static uint64_t first_due_us;           // 0: no delayed release pending

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
        // volatile: GCC drops a malloc whose block is only freed, and takes it as a success.
        void *volatile p = malloc(mid);
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
    // Allocate a hole, then a separator right after it (in address order), and so on. Then
    // fill everything else and free the holes; each is bordered by held memory above, so they
    // can't merge. malloc serves a small request from any free fragment that fits, so a
    // separator that doesn't land right after its hole is kept (that fragment is then used up)
    // and another asked for.
    void *holes[32];
    if (want > 32)
        want = 32;
    int made = 0;
    while (made < want && count < MEMHOLD_MAX_BLOCKS - 1) {
        char *h = malloc(size);
        if (!h)
            break;
        bool placed = false;
        while (!placed && count < MEMHOLD_MAX_BLOCKS - 1) {
            char *s = malloc(64);
            if (!keep_block(s, 64))
                break;
            placed = s > h && s <= h + size + 32;   // the next chunk (malloc adds a header)
        }
        if (!placed) {
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
    first_due_us = 0;
    size_t freed = 0;
    while (count > 0) {
        count--;
        free(blocks[count]);
        freed += sizes[count];
        blocks[count] = NULL;
    }
    return freed;
}

static size_t free_first(void) {
    if (count == 0)
        return 0;
    size_t n = sizes[0];
    free(blocks[0]);
    count--;
    for (int i = 0; i < count; i++) {
        blocks[i] = blocks[i + 1];
        sizes[i] = sizes[i + 1];
    }
    blocks[count] = NULL;
    return n;
}

size_t memhold_release_first(unsigned delay_ms) {
    if (count == 0)
        return 0;
    if (delay_ms == 0)
        return free_first();
    first_due_us = time_us_64() + (uint64_t)delay_ms * 1000;
    if (first_due_us == 0)
        first_due_us = 1;
    return sizes[0];
}

void memhold_poll(void) {
    if (first_due_us && time_us_64() >= first_due_us) {
        first_due_us = 0;
        free_first();
    }
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
