// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/heapwalk.c, `health heap-walk`'s walk of newlib's heap (docs/design/
// health.md §9.5), over an arena built in a buffer in newlib's layout: a clean one, then each
// rule broken in turn.

#include <string.h>

#include "core/heapwalk.h"
#include "test.h"

#define ARENA 0x20000000u
#define ARENA_SIZE 8192u
#define AV 0x20010000u
#define AV_WORDS (2 * HEAPWALK_BINS + 2)

static uint32_t arena[ARENA_SIZE / 4];
static uint32_t av[AV_WORDS];
static unsigned reads;
static bool time_up;

static uint32_t *word(uint32_t addr) {
    if (addr >= ARENA && addr < ARENA + ARENA_SIZE)
        return &arena[(addr - ARENA) / 4];
    if (addr >= AV && addr < AV + sizeof(av))
        return &av[(addr - AV) / 4];
    static uint32_t junk;
    junk = 0xdeadbeef;
    return &junk;
}

static uint32_t rd(void *ctx, uint32_t addr) {
    (void)ctx;
    reads++;
    return *word(addr);
}

static bool over(void *ctx) {
    (void)ctx;
    return time_up;
}

static void wr(uint32_t addr, uint32_t v) {
    *word(addr) = v;
}

static uint32_t bin(unsigned i) {
    return AV + 8u * i;
}

// A: in use 32 | B: free 48 | C: in use 24 | D: free 64 | E: in use 16 | top to the end.
#define A 0x20000000u
#define B (A + 32)
#define C (B + 48)
#define D (C + 24)
#define E (D + 64)
#define TOP (E + 16)
#define END (ARENA + ARENA_SIZE)

static void chunk(uint32_t p, uint32_t size, bool prev_inuse) {
    wr(p + 4, size | (prev_inuse ? 1u : 0u));
}

static void link_one(unsigned i, uint32_t node) {
    wr(bin(i) + 8, node);
    wr(bin(i) + 12, node);
    wr(node + 8, bin(i));
    wr(node + 12, bin(i));
}

static heapwalk_view_t view;

static void build(void) {
    memset(arena, 0, sizeof(arena));
    for (unsigned i = 0; i < HEAPWALK_BINS; i++) {     // every bin empty: it points at itself
        wr(bin(i) + 8, bin(i));
        wr(bin(i) + 12, bin(i));
    }
    chunk(A, 32, true);
    chunk(B, 48, true);
    chunk(C, 24, false);
    wr(C, 48);                                          // B's size repeated
    chunk(D, 64, true);
    chunk(E, 16, false);
    wr(E, 64);
    chunk(TOP, END - TOP, true);
    wr(AV + 8, TOP);                                    // av[2]: the top
    link_one(5, B);
    link_one(9, D);
    view = (heapwalk_view_t){NULL, rd, over, A, TOP, END, AV, A};
    time_up = false;
}

static heapwalk_result_t r;

TEST(clean_arena) {
    build();
    heapwalk_run(&view, 32, &r);
    CHECK_EQ(r.error, HEAPWALK_OK);
    CHECK_EQ(r.chunks, 5);
    CHECK_EQ(r.free_chunks, 2);
    CHECK_EQ(r.free_bytes, 112);
    CHECK_EQ(r.largest_free, 64);
    CHECK_EQ(r.used_bytes, 72);
    CHECK_EQ(r.bin_nodes, 2);
    CHECK_EQ(r.bin_bytes, 112);
    CHECK(r.known_found && r.known_inuse);
    CHECK_EQ(r.known_size, 32);
    CHECK_EQ(heapwalk_chunk_size(100), 104);
    CHECK_EQ(heapwalk_chunk_size(1), 16);
    CHECK_EQ(heapwalk_chunk_size(28), 32);
}

TEST(two_in_one_bin) {
    build();
    // B and D both in bin 5: B <-> D.
    link_one(9, D);
    wr(bin(9) + 8, bin(9));
    wr(bin(9) + 12, bin(9));
    wr(bin(5) + 8, B);
    wr(B + 8, D);
    wr(D + 12, B);
    wr(D + 8, bin(5));
    wr(bin(5) + 12, D);
    wr(B + 12, bin(5));
    heapwalk_run(&view, 32, &r);
    CHECK_EQ(r.error, HEAPWALK_OK);
    CHECK_EQ(r.bin_nodes, 2);
}

TEST(bad_sizes) {
    build();
    chunk(C, 20, false);                                // not a multiple of 8
    heapwalk_run(&view, 0, &r);
    CHECK_EQ(r.error, HEAPWALK_BAD_SIZE);
    CHECK_EQ(r.at, C);
    build();
    chunk(C, 8, false);                                 // under 16
    heapwalk_run(&view, 0, &r);
    CHECK_EQ(r.error, HEAPWALK_BAD_SIZE);
    build();
    chunk(E, 0x100000, false);                          // past the top
    heapwalk_run(&view, 0, &r);
    CHECK_EQ(r.error, HEAPWALK_BAD_SIZE);
    CHECK_EQ(r.at, E);
}

TEST(bad_footer) {
    build();
    wr(C, 40);
    heapwalk_run(&view, 0, &r);
    CHECK_EQ(r.error, HEAPWALK_BAD_FOOTER);
    CHECK_EQ(r.at, B);
}

TEST(adjacent_free) {
    build();
    chunk(D, 64, false);                                // C marked free too
    wr(D, 24);
    heapwalk_run(&view, 0, &r);
    CHECK_EQ(r.error, HEAPWALK_ADJACENT_FREE);
    CHECK_EQ(r.at, C);
}

TEST(top_end) {
    build();
    chunk(TOP, END - TOP - 8, true);
    heapwalk_run(&view, 0, &r);
    CHECK_EQ(r.error, HEAPWALK_TOP_END);
    build();
    view.first = TOP + 8;
    heapwalk_run(&view, 0, &r);
    CHECK_EQ(r.error, HEAPWALK_TOP_MISSED);
}

TEST(bin_pointer_outside) {
    build();
    wr(bin(5) + 8, 0x00000c41);
    heapwalk_run(&view, 0, &r);
    CHECK_EQ(r.error, HEAPWALK_BIN_POINTER);
    CHECK_EQ(r.bin, 5);
    CHECK_EQ(r.at, 0x00000c41);
    build();
    wr(bin(5) + 8, B + 4);                              // misaligned
    heapwalk_run(&view, 0, &r);
    CHECK_EQ(r.error, HEAPWALK_BIN_POINTER);
}

TEST(bin_names_chunk_in_use) {
    build();
    wr(bin(5) + 8, C);
    wr(C + 12, bin(5));
    heapwalk_run(&view, 0, &r);
    CHECK_EQ(r.error, HEAPWALK_BIN_INUSE);
    CHECK_EQ(r.at, C);
}

TEST(bin_backlink) {
    build();
    wr(B + 12, bin(6));
    heapwalk_run(&view, 0, &r);
    CHECK_EQ(r.error, HEAPWALK_BIN_BACKLINK);
    build();
    wr(bin(9) + 12, bin(9));                            // the head's back pointer
    heapwalk_run(&view, 0, &r);
    CHECK_EQ(r.error, HEAPWALK_BIN_BACKLINK);
    CHECK_EQ(r.bin, 9);
}

TEST(bin_cycle) {
    build();
    wr(B + 8, B);                                       // B points at itself: never back to the head
    wr(B + 12, bin(5));
    heapwalk_run(&view, 0, &r);
    CHECK(r.error == HEAPWALK_BIN_CYCLE || r.error == HEAPWALK_BIN_BACKLINK);
    CHECK(reads < 10000);                               // it ended
}

TEST(views_differ) {
    build();
    wr(bin(9) + 8, bin(9));                             // D missing from every list
    wr(bin(9) + 12, bin(9));
    heapwalk_run(&view, 0, &r);
    CHECK_EQ(r.error, HEAPWALK_VIEWS_DIFFER);
}

TEST(known_block) {
    build();
    heapwalk_run(&view, 40, &r);                        // the wrong size: layout not recognised
    CHECK_EQ(r.error, HEAPWALK_KNOWN_MISSING);
    build();
    view.known = B;                                     // free, not in use
    heapwalk_run(&view, 0, &r);
    CHECK_EQ(r.error, HEAPWALK_KNOWN_MISSING);
    build();
    view.known = A + 8;                                 // not a chunk at all
    heapwalk_run(&view, 0, &r);
    CHECK_EQ(r.error, HEAPWALK_KNOWN_MISSING);
}

TEST(time_runs_out) {
    // 300 chunks of 16 bytes: the time is asked at the 256th.
    memset(arena, 0, sizeof(arena));
    for (uint32_t i = 0; i < 300; i++)
        chunk(ARENA + 16 * i, 16, true);
    uint32_t top = ARENA + 16 * 300;
    chunk(top, END - top, true);
    for (unsigned i = 0; i < HEAPWALK_BINS; i++) {
        wr(bin(i) + 8, bin(i));
        wr(bin(i) + 12, bin(i));
    }
    view = (heapwalk_view_t){NULL, rd, over, ARENA, top, END, AV, 0};
    time_up = true;
    heapwalk_run(&view, 0, &r);
    CHECK_EQ(r.error, HEAPWALK_STOPPED);
    CHECK_EQ(r.chunks, 255);
    time_up = false;
    heapwalk_run(&view, 0, &r);
    CHECK_EQ(r.error, HEAPWALK_OK);
    CHECK_EQ(r.chunks, 300);
    CHECK(!strcmp(heapwalk_error_text(HEAPWALK_OK), "every rule holds"));
}

TEST_MAIN(
    RUN(clean_arena);
    RUN(two_in_one_bin);
    RUN(bad_sizes);
    RUN(bad_footer);
    RUN(adjacent_free);
    RUN(top_end);
    RUN(bin_pointer_outside);
    RUN(bin_names_chunk_in_use);
    RUN(bin_backlink);
    RUN(bin_cycle);
    RUN(views_differ);
    RUN(known_block);
    RUN(time_runs_out);
)
