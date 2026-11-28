// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// gz: a gzip file's bytes in, the data out, on zlib's inflate (docs/design/archives.md §5.2).
// Pure C, no FatFs or SDK: gunzip and zcat (files/gzip.c) feed it what they read from the card;
// tests/test_gz.c and the fuzz target feed it files made by GNU gzip, in pieces of every size.
//
// What it accepts is what GNU gunzip accepts: one gzip member or several one after the other
// (`cat a.gz b.gz > c.gz` gives a's data then b's), each checked by zlib against its CRC-32 and
// length; zero bytes after the last member (block padding) are accepted and reported
// (gz_t.trailing_zeros); anything else after a member that doesn't start another is an error.
// The header's name and date are skipped. Input can come in pieces of any size, from one byte
// up, and output is made into the caller's buffer, never more than it holds per call.

#ifndef SOS_GZ_H
#define SOS_GZ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zlib.h"

// Heap a decoder takes, roughly: zlib's inflate state (~7 KB) and its 32 KB window (allocated
// at the first output), under HEAP_ZLIB. The caller's buffers come on top.
#define GZ_HEAP (40u * 1024u)

typedef enum {
    GZ_MAGIC,       // at the start of a member: its first two bytes (1f 8b) not seen yet
    GZ_MEMBER,      // inside a member (zlib has it)
    GZ_ZEROS,       // in zero bytes after the last member
    GZ_FAILED,      // an error (gz_t.error says which); nothing more is decoded
} gz_state_t;

typedef struct {
    z_stream z;
    gz_state_t state;
    uint8_t magic[2];       // a member's first bytes, until both are seen (GZ_MAGIC)
    uint8_t magic_len;
    bool inited;            // inflateInit2() succeeded: gz_end() must free it
    bool trailing_zeros;    // zero bytes followed the last member (GNU: "ignoring trailing zeros")
    unsigned members;       // members decoded completely
    uint64_t in_total;      // input bytes taken
    uint64_t out_total;     // output bytes made
    const char *error;      // GZ_FAILED: what is wrong (static text)
} gz_t;

// Start a decoder. False (with g->error set) if zlib couldn't allocate its state.
bool gz_init(gz_t *g);

// Decode from (*in, *in_len) into out (out_cap bytes, at least 1). Takes input until it has all
// been taken, `out` is full or an error is found; advances *in and *in_len past what it took and
// sets *out_len to the bytes made. Returns false on an error (g->error), and then on every later
// call; the output made before the error was found has been given out by then.
bool gz_feed(gz_t *g, const uint8_t **in, size_t *in_len, uint8_t *out, size_t out_cap,
             size_t *out_len);

// Data held inside zlib, made from input already taken, that a call with no input would give
// out: true while the last call stopped because `out` was full. Call gz_feed() with no input
// until this is false before gz_finish().
bool gz_pending(const gz_t *g);

// The input has ended: true if it ended cleanly (after a whole member, or in trailing zeros),
// false with g->error set if not (in the middle of a member, an empty file, or an earlier error).
bool gz_finish(gz_t *g);

// Free zlib's state. Safe after a failed gz_init() and more than once.
void gz_end(gz_t *g);

// gunzip's output name for the input `name` (a path or a bare name): "<x>.gz" gives "<x>", and
// "<x>.tgz" gives "<x>.tar", the suffix matched in any case (FAT names are case-insensitive), and
// <x> not empty. False for any other name ("unknown suffix") or if the result doesn't fit in
// `out` (size `n`).
bool gz_output_name(const char *name, char *out, size_t n);

// zlib's allocator for SOS (Z_SOLO has none of its own): the heap, under HEAP_ZLIB. Also for
// deflate (gzip).
void *gz_zalloc(void *opaque, unsigned items, unsigned size);
void gz_zfree(void *opaque, void *p);

#endif
