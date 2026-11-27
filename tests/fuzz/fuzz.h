// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Each fuzz_<name>.c defines LLVMFuzzerTestOneInput: built with -fsanitize=fuzzer it is a
// libFuzzer target (just fuzz <name>); linked with replay.c it runs the files in
// tests/fuzz/corpus/<name>/ once each, as a host test.

#ifndef SOS_FUZZ_H
#define SOS_FUZZ_H

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

// Take the next byte of the input (0 when it runs out): for choices a target makes.
static inline uint8_t fuzz_byte(const uint8_t **data, size_t *size) {
    if (!*size)
        return 0;
    (*size)--;
    return *(*data)++;
}

#endif
