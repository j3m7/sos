// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// src/files/gz.c: what gunzip and zcat read from a file on the card, which can hold anything.
// Any input is decoded or refused, and the same way whatever the pieces it comes in and the
// output buffer: decoded once in one piece into gunzip's 4 KB buffer, and once in pieces and
// into a buffer chosen by the first two bytes, both give the same verdict, the same error and
// the same output (compared by length and CRC-32). No call makes more than its buffer holds or
// stops with input left unless its buffer is full. Output past OUT_MAX isn't compared (a small
// input can inflate a thousandfold; the decoder's bound per call is what matters).
//
// And src/files/gzw.c, gzip's encoder: the same bytes, compressed in those pieces into that
// buffer at a level chosen by the first byte, decode back to themselves.

#include <stdlib.h>
#include <string.h>

#include "core/heaptag.h"
#include "files/gz.h"
#include "files/gzw.h"
#include "fuzz.h"

// Checked in every build (NDEBUG or not).
#define MUST(x) do { if (!(x)) abort(); } while (0)

#define OUT_MAX (1u << 20)

typedef struct {
    bool ok, cut;               // cut: stopped at OUT_MAX, no verdict
    const char *error;
    bool zeros;
    uint64_t len;
    uLong crc;
} run_t;

static run_t decode(const uint8_t *data, size_t size, size_t piece, size_t cap) {
    static uint8_t buf[4096];
    run_t r = {.crc = crc32(0, NULL, 0)};
    gz_t g;
    MUST(gz_init(&g));
    size_t pos = 0;
    bool failed = false;
    while (!failed && !r.cut) {
        size_t n = size - pos < piece ? size - pos : piece;
        const uint8_t *p = data + pos;
        size_t left = n;
        if (!n && !gz_pending(&g))
            break;
        do {
            size_t made;
            bool fed = gz_feed(&g, &p, &left, buf, cap, &made);
            MUST(made <= cap);
            MUST(!fed || !left || made == cap);
            size_t take = r.len + made > OUT_MAX ? OUT_MAX - r.len : made;
            r.crc = crc32(r.crc, buf, (uInt)take);
            r.len += take;
            if (r.len == OUT_MAX)
                r.cut = true;
            failed = !fed;
            if (failed)
                MUST(g.error && g.state == GZ_FAILED);
        } while (!failed && !r.cut && (left || gz_pending(&g)));
        pos += n - left;
    }
    if (!r.cut) {
        r.ok = !failed && gz_finish(&g);
        r.error = r.ok ? NULL : g.error;
        MUST(r.ok || r.error);
        MUST(g.in_total <= size);
        r.zeros = g.trailing_zeros;
    }
    gz_end(&g);
    MUST(heap_tag_used(HEAP_ZLIB) == 0);
    return r;
}

// Compress `data` with gzw.c (in pieces of `piece`, into `cap` bytes at a time) and decode the
// result: it must give `data` back.
static void round_trip(const uint8_t *data, size_t size, size_t piece, size_t cap, int level) {
    static uint8_t gz[16384], back[8192 + 1];   // + 1: room for a call that finds no more
    static uint8_t out[4096];
    gzw_t w;
    MUST(gzw_init(&w, level));
    size_t pos = 0, len = 0;
    while (!gzw_done(&w)) {
        size_t n = size - pos < piece ? size - pos : piece;
        const uint8_t *p = data + pos;
        size_t left = n;
        bool last = pos + n == size;
        size_t made;
        do {
            MUST(gzw_feed(&w, &p, &left, last, out, cap, &made));
            MUST(made <= cap && (!left || made == cap));
            MUST(len + made <= sizeof(gz));
            memcpy(gz + len, out, made);
            len += made;
        } while (!gzw_done(&w) && (left || made == cap));
        pos += n;
    }
    MUST(w.in_total == size && w.out_total == len);
    gzw_end(&w);
    MUST(heap_tag_used(HEAP_ZLIB) == 0);

    gz_t g;
    MUST(gz_init(&g));
    const uint8_t *p = gz;
    size_t left = len, got = 0;
    while (left || gz_pending(&g)) {
        size_t made;
        MUST(gz_feed(&g, &p, &left, back + got, sizeof(back) - got, &made));
        got += made;
        MUST(got <= size);
    }
    MUST(gz_finish(&g) && got == size && !memcmp(back, data, size));
    gz_end(&g);
    MUST(heap_tag_used(HEAP_ZLIB) == 0);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // [piece - 1][buffer / 16 - 1] then the file.
    size_t piece = (size_t)fuzz_byte(&data, &size) + 1;
    size_t cap = ((size_t)fuzz_byte(&data, &size) + 1) * 16;
    if (size <= 8192)
        round_trip(data, size, piece, cap, (int)(piece % 9) + 1);
    run_t a = decode(data, size, size ? size : 1, 4096);
    run_t b = decode(data, size, piece, cap);
    if (a.cut || b.cut) {
        MUST(a.cut && b.cut && a.crc == b.crc);
        return 0;
    }
    MUST(a.ok == b.ok && a.len == b.len && a.crc == b.crc && a.zeros == b.zeros);
    MUST(a.ok || !strcmp(a.error, b.error));
    return 0;
}
