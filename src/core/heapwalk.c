// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See heapwalk.h.

#include "core/heapwalk.h"

#include <string.h>

#define SIZE_SZ     4u
#define ALIGN_MASK  7u
#define MINSIZE     16u
#define PREV_INUSE  1u
#define SIZE_BITS   3u                  // PREV_INUSE | IS_MMAPPED

static uint32_t rd(const heapwalk_view_t *v, uint32_t addr) {
    return v->read(v->ctx, addr);
}

static uint32_t chunksize(const heapwalk_view_t *v, uint32_t p) {
    return rd(v, p + SIZE_SZ) & ~SIZE_BITS;
}

uint32_t heapwalk_chunk_size(uint32_t n) {
    uint32_t s = (n + SIZE_SZ + ALIGN_MASK) & ~ALIGN_MASK;
    return s < MINSIZE ? MINSIZE : s;
}

static bool fail(heapwalk_result_t *r, heapwalk_error_t e, uint32_t at, unsigned bin) {
    r->error = e;
    r->at = at;
    r->bin = bin;
    return false;
}

static bool walk_chunks(const heapwalk_view_t *v, heapwalk_result_t *r) {
    bool prev_free = false;
    for (uint32_t p = v->first; p < v->top;) {
        if ((r->chunks & 255u) == 255u && v->over && v->over(v->ctx))
            return fail(r, HEAPWALK_STOPPED, p, 0);
        uint32_t size = chunksize(v, p);
        if (size % 8 || size < MINSIZE || size > v->top - p)
            return fail(r, HEAPWALK_BAD_SIZE, p, 0);
        uint32_t next = p + size;
        bool inuse = rd(v, next + SIZE_SZ) & PREV_INUSE;
        r->chunks++;
        if (p == v->known) {
            r->known_found = true;
            r->known_size = size;
            r->known_inuse = inuse;
        }
        if (inuse) {
            r->used_bytes += size;
            prev_free = false;
            p = next;
            continue;
        }
        if (rd(v, next) != size)
            return fail(r, HEAPWALK_BAD_FOOTER, p, 0);
        if (prev_free)
            return fail(r, HEAPWALK_ADJACENT_FREE, p, 0);
        prev_free = true;
        r->free_chunks++;
        r->free_bytes += size;
        if (size > r->largest_free)
            r->largest_free = size;
        p = next;
    }
    return true;
}

static bool walk_bins(const heapwalk_view_t *v, heapwalk_result_t *r) {
    for (unsigned i = 1; i < HEAPWALK_BINS; i++) {
        uint32_t bin = v->av + 8u * i;          // the fake chunk whose fd is av[2i + 2]
        uint32_t prev = bin, node = rd(v, bin + 2 * SIZE_SZ);
        while (node != bin) {
            if (node < v->first || node >= v->top || node & ALIGN_MASK)
                return fail(r, HEAPWALK_BIN_POINTER, node, i);
            uint32_t size = chunksize(v, node);
            if (size % 8 || size < MINSIZE || size > v->top - node)
                return fail(r, HEAPWALK_BIN_INUSE, node, i);
            if (rd(v, node + size + SIZE_SZ) & PREV_INUSE)
                return fail(r, HEAPWALK_BIN_INUSE, node, i);
            if (rd(v, node + 3 * SIZE_SZ) != prev)
                return fail(r, HEAPWALK_BIN_BACKLINK, node, i);
            if (++r->bin_nodes > r->free_chunks)
                return fail(r, HEAPWALK_BIN_CYCLE, node, i);
            r->bin_bytes += size;
            prev = node;
            node = rd(v, node + 2 * SIZE_SZ);
        }
        if (rd(v, bin + 3 * SIZE_SZ) != prev)
            return fail(r, HEAPWALK_BIN_BACKLINK, bin, i);
    }
    return true;
}

void heapwalk_run(const heapwalk_view_t *v, uint32_t known_size, heapwalk_result_t *r) {
    memset(r, 0, sizeof(*r));
    // Each size is checked to end at or before the top, so the walk lands on it exactly unless
    // it starts past it.
    if (v->first > v->top || (v->first ^ v->top) & ALIGN_MASK) {
        fail(r, HEAPWALK_TOP_MISSED, v->first, 0);
        return;
    }
    if (!walk_chunks(v, r))
        return;
    if (v->top + chunksize(v, v->top) != v->end) {
        fail(r, HEAPWALK_TOP_END, v->top, 0);
        return;
    }
    if (!walk_bins(v, r))
        return;
    if (r->bin_nodes != r->free_chunks || r->bin_bytes != r->free_bytes) {
        fail(r, HEAPWALK_VIEWS_DIFFER, 0, 0);
        return;
    }
    if (v->known && (!r->known_found || !r->known_inuse ||
                     (known_size && r->known_size != known_size)))
        fail(r, HEAPWALK_KNOWN_MISSING, v->known, 0);
}

const char *heapwalk_error_text(heapwalk_error_t e) {
    static const char *const text[] = {
        [HEAPWALK_OK] = "every rule holds",
        [HEAPWALK_STOPPED] = "stopped: the time ran out (not judged)",
        [HEAPWALK_BAD_SIZE] = "a size not a multiple of 8, under 16, or past the top",
        [HEAPWALK_BAD_FOOTER] = "a free chunk's size not repeated after it",
        [HEAPWALK_ADJACENT_FREE] = "two free chunks side by side",
        [HEAPWALK_TOP_MISSED] = "the walk didn't land on the top chunk",
        [HEAPWALK_TOP_END] = "the top chunk doesn't end at sbrk(0)",
        [HEAPWALK_BIN_POINTER] = "a free list pointer outside the arena or misaligned",
        [HEAPWALK_BIN_INUSE] = "a free list naming a chunk in use",
        [HEAPWALK_BIN_BACKLINK] = "a free list's back pointer not naming the node before",
        [HEAPWALK_BIN_CYCLE] = "more free list nodes than free chunks (a cycle?)",
        [HEAPWALK_VIEWS_DIFFER] = "the walk and the free lists disagree",
        [HEAPWALK_KNOWN_MISSING] = "the known block isn't in use at its size: layout unknown",
    };
    return (unsigned)e < sizeof(text) / sizeof(text[0]) ? text[e] : "?";
}
