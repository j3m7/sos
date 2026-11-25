// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Terminal column widths (see charwidth.h): a binary search in two sorted range tables.

#include "shell/charwidth.h"

#include <stddef.h>

typedef struct {
    uint32_t first, last;
} charwidth_range_t;

#include "shell/charwidth_table.h"

static int in(const charwidth_range_t *t, size_t n, uint32_t cp) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < t[mid].first)
            hi = mid;
        else if (cp > t[mid].last)
            lo = mid + 1;
        else
            return 1;
    }
    return 0;
}

#define COUNT(t) (sizeof(t) / sizeof((t)[0]))

int charwidth(uint32_t cp) {
    if (cp < 0x300)                         // ASCII and Latin-1: the common case, no search
        return 1;
    if (in(charwidth_zero, COUNT(charwidth_zero), cp))
        return 0;
    if (in(charwidth_wide, COUNT(charwidth_wide), cp))
        return 2;
    return 1;
}
