// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "core/stackmon.h"

#include <stdint.h>

#define PATTERN 0x5a5aa5a5u

extern uint32_t __StackBottom[], __StackTop[];   // the region's ends (linker script)

void __attribute__((noinline)) stackmon_paint(void) {
    uint32_t here;
    // Leave a margin below the current frame for this function and anything an interrupt pushes.
    uint32_t *limit = (uint32_t *)(((uintptr_t)&here - 256) & ~3u);
    for (uint32_t *p = __StackBottom; p < limit; p++)
        *p = PATTERN;
}

size_t stackmon_peak(void) {
    const uint32_t *p = __StackBottom;
    while (p < __StackTop && *p == PATTERN)
        p++;
    return (size_t)((uintptr_t)__StackTop - (uintptr_t)p);
}

size_t stackmon_size(void) {
    return (size_t)((uintptr_t)__StackTop - (uintptr_t)__StackBottom);
}
