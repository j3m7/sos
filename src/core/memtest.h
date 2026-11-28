// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// A march test of RAM for the `ram` health check (docs/design/health.md §9.7). Pure C: it reads
// and writes the words it is given and nothing else, so tests/test_memtest.c runs it over a
// simulated memory with faults in it.
//
// March C- (van de Goor): {w0}; up(r0,w1); up(r1,w0); down(r0,w1); down(r1,w0); {r0}, ten
// accesses a word. It finds stuck-at, transition and address decoder faults, and the unlinked
// coupling faults (inversion, idempotent, state) between two words. Because memory is read and
// written a word at a time, coupling between two bits of one word needs more than all-0 and
// all-1: the test runs over six data backgrounds (0, 0x55555555, 0x33333333, 0x0f0f0f0f,
// 0x00ff00ff, 0x0000ffff; "1" is the complement), log2(32) + 1 of them. 60 accesses a word.
//
// It can't find retention faults (it reads back within microseconds), faults that depend on the
// temperature or supply at another moment, or anything in memory it isn't given.

#ifndef SOS_MEMTEST_H
#define SOS_MEMTEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MEMTEST_BACKGROUNDS 6
#define MEMTEST_ELEMENTS    6           // March C-'s elements, numbered 0 to 5 as above

// The first word read back wrong.
typedef struct {
    size_t index;                       // word index into the memory tested
    uint32_t wrote, read;
    uint8_t element;                    // 1 to 5: the element whose read saw it
    uint32_t background;
} memtest_fault_t;

// Called once per run, between the first element's writes and the second element's reads
// (first background only), to disturb the memory on purpose: `health fault ram` (test builds)
// flips a bit there. NULL for none.
typedef void (*memtest_disturb_fn)(void *arg, volatile uint32_t *words, size_t n);

// March C- over n words, under each background in turn. True if every read gave what was
// written; otherwise false, with the first wrong read in *fault, and the memory left as the test
// left it (the caller owns it, or puts it back).
bool memtest_march(volatile uint32_t *words, size_t n, memtest_disturb_fn disturb, void *arg,
                   memtest_fault_t *fault);

// The element's name for a log line: "r0,w1 up" and so on.
const char *memtest_element_name(unsigned element);

#endif
