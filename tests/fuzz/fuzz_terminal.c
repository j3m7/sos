// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// What a terminal (an SSH client, the serial console) sends and what reaches it:
// src/shell/keys.c decodes key sequences byte by byte, src/shell/termtext.c filters text for
// display (control characters, broken UTF-8), in pieces of a size the first byte chooses.

#include <stdlib.h>

#include "fuzz.h"
#include "shell/keys.h"
#include "shell/termtext.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    size_t piece = (size_t)fuzz_byte(&data, &size) % 16 + 1;

    keys_t k = KEYS_INIT;
    for (size_t i = 0; i < size; i++)
        keys_feed(&k, data[i]);

    termtext_t t = {0};
    char out[16 + 3];                               // termtext_filter: len + 3
    for (size_t i = 0; i < size; i += piece) {
        size_t n = size - i < piece ? size - i : piece;
        if (termtext_filter(&t, data + i, n, out) > n + 3)
            abort();
    }
    if (termtext_flush(&t, out) > 3)
        abort();
    return 0;
}
