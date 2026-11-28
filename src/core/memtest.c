// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See memtest.h. Every access goes through RD() and WR(): volatile accesses on the board, and in
// tests/test_memtest.c (built with MEMTEST_HOOKS) calls into a simulated memory with faults.

#include "core/memtest.h"

#ifdef MEMTEST_HOOKS
uint32_t memtest_hook_read(volatile uint32_t *p);
void memtest_hook_write(volatile uint32_t *p, uint32_t v);
#define RD(p)    memtest_hook_read(p)
#define WR(p, v) memtest_hook_write((p), (v))
#else
#define RD(p)    (*(p))
#define WR(p, v) (*(p) = (v))
#endif

static const uint32_t backgrounds[MEMTEST_BACKGROUNDS] = {
    0x00000000u, 0x55555555u, 0x33333333u, 0x0f0f0f0fu, 0x00ff00ffu, 0x0000ffffu,
};

static bool wrong(volatile uint32_t *words, size_t i, uint32_t want, unsigned element,
                  uint32_t bg, memtest_fault_t *fault) {
    uint32_t got = RD(&words[i]);
    if (got == want)
        return false;
    *fault = (memtest_fault_t){
        .index = i, .wrote = want, .read = got, .element = (uint8_t)element, .background = bg,
    };
    return true;
}

// One element that reads `want` and writes `put` at each word, upward or downward.
static bool read_write(volatile uint32_t *words, size_t n, bool up, uint32_t want, uint32_t put,
                       unsigned element, uint32_t bg, memtest_fault_t *fault) {
    for (size_t k = 0; k < n; k++) {
        size_t i = up ? k : n - 1 - k;
        if (wrong(words, i, want, element, bg, fault))
            return false;
        WR(&words[i], put);
    }
    return true;
}

bool memtest_march(volatile uint32_t *words, size_t n, memtest_disturb_fn disturb, void *arg,
                   memtest_fault_t *fault) {
    for (unsigned b = 0; b < MEMTEST_BACKGROUNDS; b++) {
        uint32_t zero = backgrounds[b], one = ~zero;
        for (size_t i = 0; i < n; i++)
            WR(&words[i], zero);
        if (disturb && b == 0)
            disturb(arg, words, n);
        if (!read_write(words, n, true, zero, one, 1, zero, fault) ||
            !read_write(words, n, true, one, zero, 2, zero, fault) ||
            !read_write(words, n, false, zero, one, 3, zero, fault) ||
            !read_write(words, n, false, one, zero, 4, zero, fault))
            return false;
        for (size_t i = 0; i < n; i++)
            if (wrong(words, i, zero, 5, zero, fault))
                return false;
    }
    return true;
}

const char *memtest_element_name(unsigned element) {
    static const char *const names[MEMTEST_ELEMENTS] = {
        "w0", "r0,w1 up", "r1,w0 up", "r0,w1 down", "r1,w0 down", "r0",
    };
    return element < MEMTEST_ELEMENTS ? names[element] : "?";
}
