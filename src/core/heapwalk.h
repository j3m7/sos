// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// `health heap-walk` (test builds only, run by hand): a walk of newlib's heap that checks every
// chunk and every free list (docs/design/health.md §9.5). Not a background check: it depends on
// newlib's internals (mallocr.c, Doug Lea's malloc 2.6.4), which a toolchain update could change,
// and it holds the allocator's lock while it runs. A person reads what it prints.
//
// The layout it assumes (32-bit, MALLOC_ALIGNMENT 8): a chunk is [prev_size][size | PREV_INUSE]
// then the user's bytes; a free chunk holds its bin's forward and back pointers after the size,
// and repeats its size in the next chunk's prev_size. Chunks lie end to end from the arena's first
// to the top chunk, which ends at sbrk(0). The bins are newlib's __malloc_av_: bin i's list head is
// a fake chunk at av + 8 * i whose forward and back pointers are av[2i + 2] and av[2i + 3]; bin 0
// is the top, bins 1 to 127 hold free chunks.
//
// heapwalk.c is pure: it reads memory through a callback with 32-bit addresses, so the host tests
// run it over arenas built in a buffer. The board's part is in bgcheck_board.c.

#ifndef SOS_HEAPWALK_H
#define SOS_HEAPWALK_H

#include <stdbool.h>
#include <stdint.h>

#define HEAPWALK_BINS 128

typedef struct {
    void *ctx;
    uint32_t (*read)(void *ctx, uint32_t addr);     // one aligned word
    bool (*over)(void *ctx);                        // the time is up (asked every 256 chunks)
    uint32_t first;                                 // the first chunk
    uint32_t top;                                   // the top chunk (av[2])
    uint32_t end;                                   // sbrk(0)
    uint32_t av;                                    // __malloc_av_
    uint32_t known;                                 // a chunk known to be in use, 0: none
} heapwalk_view_t;

typedef enum {
    HEAPWALK_OK,
    HEAPWALK_STOPPED,           // the time ran out (not judged)
    HEAPWALK_BAD_SIZE,          // a size not a multiple of 8, under 16, or past the top
    HEAPWALK_BAD_FOOTER,        // a free chunk's size not repeated after it
    HEAPWALK_ADJACENT_FREE,     // two free chunks side by side (newlib merges them)
    HEAPWALK_TOP_MISSED,        // the walk didn't land on the top chunk
    HEAPWALK_TOP_END,           // the top chunk doesn't end at sbrk(0)
    HEAPWALK_BIN_POINTER,       // a list pointer outside the arena or misaligned
    HEAPWALK_BIN_INUSE,         // a list naming a chunk in use
    HEAPWALK_BIN_BACKLINK,      // a back pointer not naming the node before
    HEAPWALK_BIN_CYCLE,         // more list nodes than free chunks
    HEAPWALK_VIEWS_DIFFER,      // the walk and the lists disagree on the free chunks
    HEAPWALK_KNOWN_MISSING,     // the known block wasn't found in use at its size
} heapwalk_error_t;

typedef struct {
    heapwalk_error_t error;
    uint32_t at;                // the chunk (or list node) where it went wrong
    unsigned bin;               // ...in this bin, for the BIN_* errors
    uint32_t chunks, free_chunks;
    uint32_t used_bytes, free_bytes, largest_free;
    uint32_t bin_nodes, bin_bytes;
    bool known_found;           // the known chunk was met
    uint32_t known_size;        // ...with this size
    bool known_inuse;
} heapwalk_result_t;

// Walk the chunks, then the lists; stops at the first rule broken. known_size: the size the known
// chunk must have (0: don't check).
void heapwalk_run(const heapwalk_view_t *v, uint32_t known_size, heapwalk_result_t *r);

// "a size not a multiple of 8, under 16, or past the top", ...
const char *heapwalk_error_text(heapwalk_error_t e);

// newlib's chunk size for a request of n bytes (request2size): n + 4 rounded up to 8, at least 16.
uint32_t heapwalk_chunk_size(uint32_t n);

#endif
