// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/memtest.c, the `ram` check's march test (docs/design/health.md §9.7).
// memtest.c is built with MEMTEST_HOOKS, so its reads and writes come here: a simulated memory
// of N words in which one fault at a time is made, each of the kinds March C- with six
// backgrounds must find. A memory without faults must pass.

#include <stdint.h>
#include <string.h>

#include "core/memtest.h"
#include "test.h"

#define N 64

static volatile uint32_t mem[N];        // the addresses memtest is given
static uint32_t cell[N];                // what the simulated cells hold

typedef enum {
    F_NONE,
    F_STUCK,        // bit `bit` of word `a` stuck at `val`
    F_TRANSITION,   // bit `bit` of word `a` can't go from 0 to 1 (val 1) or 1 to 0 (val 0)
    F_CF_INV,       // a transition of bit `bit` in word `a` inverts bit `vbit` of word `v`
    F_CF_ID,        // bit `bit` of word `a` going up forces bit `vbit` of word `v` to `val`
    F_DECODER,      // address `a` reaches cell `v` instead of its own
} fault_kind_t;

static struct {
    fault_kind_t kind;
    size_t a, v;
    unsigned bit, vbit;
    uint32_t val;
} f;

static size_t where(volatile uint32_t *p) {
    size_t i = (size_t)(p - mem);
    if (f.kind == F_DECODER && i == f.a)
        return f.v;
    return i;
}

static uint32_t stuck(size_t i, uint32_t v) {
    if (f.kind == F_STUCK && i == f.a)
        v = f.val ? v | 1u << f.bit : v & ~(1u << f.bit);
    return v;
}

uint32_t memtest_hook_read(volatile uint32_t *p);
void memtest_hook_write(volatile uint32_t *p, uint32_t v);

uint32_t memtest_hook_read(volatile uint32_t *p) {
    size_t i = where(p);
    return stuck(i, cell[i]);
}

void memtest_hook_write(volatile uint32_t *p, uint32_t v) {
    size_t i = where(p);
    uint32_t old = cell[i], m = 1u << f.bit;
    if (f.kind == F_TRANSITION && i == f.a) {
        if (f.val && !(old & m))
            v &= ~m;                    // stays 0
        if (!f.val && (old & m))
            v |= m;                     // stays 1
    }
    cell[i] = stuck(i, v);
    if (i == f.a && (old ^ cell[i]) & m) {
        uint32_t vm = 1u << f.vbit;
        if (f.kind == F_CF_INV)
            cell[f.v] ^= vm;
        if (f.kind == F_CF_ID && (cell[i] & m))
            cell[f.v] = f.val ? cell[f.v] | vm : cell[f.v] & ~vm;
    }
}

static bool run(memtest_fault_t *got) {
    memset(cell, 0xa5, sizeof(cell));
    return memtest_march(mem, N, NULL, NULL, got);
}

TEST(clean_memory_passes) {
    f.kind = F_NONE;
    memtest_fault_t got;
    CHECK(run(&got));
}

TEST(stuck_at_every_bit) {
    for (unsigned bit = 0; bit < 32; bit++)
        for (uint32_t val = 0; val < 2; val++) {
            f.kind = F_STUCK;
            f.a = 17;
            f.bit = bit;
            f.val = val;
            memtest_fault_t got;
            CHECK(!run(&got));
            CHECK_EQ(got.index, 17);
            CHECK_EQ((got.wrote ^ got.read), 1u << bit);
        }
}

TEST(transition_faults) {
    for (uint32_t val = 0; val < 2; val++) {
        f.kind = F_TRANSITION;
        f.a = 5;
        f.bit = 9;
        f.val = val;
        memtest_fault_t got;
        CHECK(!run(&got));
        CHECK_EQ(got.index, 5);
    }
}

// Aggressor below and above the victim, every bit pair from a few positions, each value.
TEST(coupling_between_words) {
    static const size_t pairs[][2] = {{3, 40}, {40, 3}, {0, 63}, {63, 0}};
    for (unsigned p = 0; p < 4; p++)
        for (unsigned kind = 0; kind < 3; kind++) {
            f.kind = kind == 0 ? F_CF_INV : F_CF_ID;
            f.val = kind == 2;
            f.a = pairs[p][0];
            f.v = pairs[p][1];
            f.bit = 7;
            f.vbit = 21;
            memtest_fault_t got;
            CHECK(!run(&got));
            CHECK_EQ(got.index, f.v);
        }
}

// Two bits of one word: needs the data backgrounds (all-0 and all-1 alone write both bits at
// once).
TEST(coupling_inside_a_word) {
    unsigned found = 0, tried = 0;
    for (unsigned bit = 0; bit < 32; bit += 5)
        for (unsigned vbit = 0; vbit < 32; vbit += 3) {
            if (bit == vbit)
                continue;
            for (unsigned kind = 0; kind < 3; kind++) {
                f.kind = kind == 0 ? F_CF_INV : F_CF_ID;
                f.val = kind == 2;
                f.a = f.v = 30;
                f.bit = bit;
                f.vbit = vbit;
                memtest_fault_t got;
                tried++;
                if (!run(&got) && got.index == 30)
                    found++;
            }
        }
    CHECK_EQ(found, tried);
}

TEST(address_decoder_fault) {
    f.kind = F_DECODER;
    f.a = 12;
    f.v = 50;
    memtest_fault_t got;
    CHECK(!run(&got));
    f.a = 50;
    f.v = 12;
    CHECK(!run(&got));
}

// The board's flip writes the word; here the word is a simulated cell.
static void flip(void *arg, volatile uint32_t *words, size_t n) {
    (void)words;
    (void)n;
    cell[*(size_t *)arg] ^= 0x400u;
}

TEST(disturbed_on_purpose) {
    f.kind = F_NONE;
    size_t at = 33;
    memtest_fault_t got;
    memset(cell, 0, sizeof(cell));
    CHECK(!memtest_march(mem, N, flip, &at, &got));
    CHECK_EQ(got.index, 33);
    CHECK_EQ(got.wrote, 0);
    CHECK_EQ(got.read, 0x400);
    CHECK_EQ(got.element, 1);
    CHECK(!strcmp(memtest_element_name(got.element), "r0,w1 up"));
    CHECK(!strcmp(memtest_element_name(9), "?"));
}

TEST_MAIN(RUN(clean_memory_passes); RUN(stuck_at_every_bit); RUN(transition_faults);
          RUN(coupling_between_words); RUN(coupling_inside_a_word); RUN(address_decoder_fault);
          RUN(disturbed_on_purpose);)
