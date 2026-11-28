// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// gzw: data in, a gzip file's bytes out, on zlib's deflate (docs/design/archives.md §5.4). The
// writing side of files/gz.h, pure C with no FatFs or SDK: gzip (files/gzip.c) feeds it what it
// reads from the card; tests/test_gz.c and the gz fuzz target feed it data and read it back with
// files/gz.h and GNU gunzip.
//
// Memory decides the settings: a 4 KB window (windowBits 12) and memLevel 6 take ~54 KB, where
// zlib's defaults take ~262 KB, most of the heap. The output is one standard gzip member that
// any gunzip reads; its header holds no name and the date 0, as `gzip -n` writes it. Input can
// come in pieces of any size and output is made into the caller's buffer, never more than it
// holds per call.

#ifndef SOS_GZW_H
#define SOS_GZW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zlib.h"

#define GZW_WINDOW_BITS 12          // a 4 KB window
#define GZW_MEM_LEVEL   6
#define GZW_LEVEL       6           // gzip's default level

// Heap an encoder takes: zlib's (1 << (windowBits + 2)) + (1 << (memLevel + 9)) and its state,
// ~54 KB under HEAP_ZLIB (measured by tests/test_gz.c, which checks this bound). The caller's
// buffers come on top.
#define GZW_HEAP (56u * 1024u)

typedef struct {
    z_stream z;
    bool inited;            // deflateInit2() succeeded: gzw_end() must free it
    bool done;              // the whole member, trailer included, has been given out
    uint64_t in_total;      // input bytes taken
    uint64_t out_total;     // output bytes made
    const char *error;      // what went wrong (static text)
} gzw_t;

// Start an encoder at `level` (1 to 9). False (with w->error set) if zlib couldn't allocate its
// state.
bool gzw_init(gzw_t *w, int level);

// Encode from (*in, *in_len) into out (out_cap bytes, at least 1); `last` says no input follows
// what is given now. Takes input until it has all been taken or `out` is full; advances *in and
// *in_len past what it took and sets *out_len to the bytes made. Call again with the input left
// (none, at the end) while `out` comes back full, and with `last` until gzw_done(). False on an
// error (w->error), which only a wrong call can cause.
bool gzw_feed(gzw_t *w, const uint8_t **in, size_t *in_len, bool last, uint8_t *out,
              size_t out_cap, size_t *out_len);

// True once the member's end has been given out: the output is complete.
bool gzw_done(const gzw_t *w);

// Free zlib's state. Safe after a failed gzw_init() and more than once.
void gzw_end(gzw_t *w);

// gzip's output name for the input `name` (a path or a bare name): "<x>" gives "<x>.gz". False
// if `name` already ends in ".gz" or ".tgz" (any case: FAT names are case-insensitive), ends in
// "/", or the result doesn't fit in `out` (size `n`).
bool gzw_output_name(const char *name, char *out, size_t n);

// Does `name` end in ".gz" or ".tgz" (any case)? gzip refuses those.
bool gzw_has_gz_suffix(const char *name);

#endif
