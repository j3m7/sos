// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The reading side of tar (docs/design/archives.md §6.1-§6.3): an archive's bytes in, its entries
// and their data out, and the rules a name must pass before anything is created under it. Pure C
// (no FatFs, no allocation), so it's unit tested and fuzzed on the host (tests/test_tar_parse.c,
// tests/fuzz/fuzz_tar.c); files/tar.c feeds it what it reads from the card, after gz.h when the
// archive is gzipped.
//
// What it reads: ustar (POSIX 1988) and GNU headers, and old v7 ones (no magic) by their checksum
// alone. Each header's checksum is checked (the unsigned sum, or the signed one some old tars
// wrote). Numbers in octal, or in GNU's base-256 when the top bit of the field is set. Long names
// from GNU 'L' records and pax 'x' records (path, size, linkpath; other keys ignored); GNU 'K'
// (a long link target) the same way; pax 'g' (global) records skipped whole, as GNU does with path
// and size there; the ustar prefix joined to the name. Two zero blocks end the archive; one zero
// block followed by a header is passed over, as GNU does, and one at the very end is taken as the
// end. Anything after the end is ignored.
//
// Input can come in pieces of any size. A name or link target that doesn't fit TAR_NAME_MAX, or
// pax records over TAR_PAX_MAX, is never truncated: the entry it belongs to comes with `refused`
// set, and the caller skips it.

#ifndef SOS_TAR_PARSE_H
#define SOS_TAR_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TAR_BLOCK    512
#define TAR_NAME_MAX (255 * 3 + 1)  // FF_MAX_LFN UTF-16 units as UTF-8, and the NUL
#define TAR_PAX_MAX  4096           // a pax 'x' record set at most

typedef enum {
    TAR_FILE,           // '0', '\0', '7' (contiguous): a regular file
    TAR_DIR,            // '5', or a v7 file whose name ends in '/'
    TAR_HARDLINK,       // '1'
    TAR_SYMLINK,        // '2'
    TAR_CHARDEV,        // '3'
    TAR_BLOCKDEV,       // '4'
    TAR_FIFO,           // '6'
    TAR_OTHER,          // anything else (GNU 'S' sparse, 'V' label, 'D' dumpdir, ...)
} tar_kind_t;

typedef struct {
    tar_kind_t kind;
    char type;                  // the type letter as found
    uint64_t size;              // the data that follows (0 for links, devices, directories)
    int64_t mtime;              // seconds since 1970 (UTC), from the header
    uint64_t offset;            // where its header starts in the archive
    const char *refused;        // non-NULL: the parser couldn't take its name (static text)
    char name[TAR_NAME_MAX];    // as the archive has it: pax path, GNU long name, prefix/name
    char link[TAR_NAME_MAX];    // a link's target, "" for other kinds
} tar_entry_t;

typedef enum {
    TAR_MORE,           // all the input was taken; feed more, or call tar_finish()
    TAR_HEADER,         // an entry starts: tar_t.entry
    TAR_DATA,           // some of the entry's data: tar_t.data, tar_t.data_len
    TAR_END,            // the archive ended (two zero blocks)
    TAR_ERROR,          // a bad header (tar_t.error, tar_t.error_at); nothing more is read
} tar_event_t;

typedef enum {
    TAR_S_HEADER,       // reading a header block
    TAR_S_DATA,         // inside an entry's data, or a long name's or pax record's
    TAR_S_PAD,          // in the zeros after data, up to the next block
    TAR_S_END,
    TAR_S_FAILED,
} tar_state_t;

typedef struct {
    tar_state_t state;
    uint64_t pos;               // archive bytes taken
    uint8_t block[TAR_BLOCK];   // the header being read
    size_t have;                // bytes of it so far
    unsigned zeros;             // zero blocks in a row just before
    uint64_t left;              // data (or padding) still to come
    char meta;                  // inside an 'L', 'K', 'x' or 'g' record's data (its type), or 0
    uint64_t meta_at, meta_size;    // that record's header offset and data size
    size_t meta_len;            // bytes of it in meta_buf
    bool meta_over;             // longer than meta_buf: taken and dropped
    // What 'L', 'K' and 'x' records said for the next header.
    bool next_name, next_link, next_size;
    const char *next_refused;
    uint64_t next_size_val;
    char next_name_buf[TAR_NAME_MAX];
    char next_link_buf[TAR_NAME_MAX];
    char meta_buf[TAR_PAX_MAX];
    tar_entry_t entry;          // the entry last given out (TAR_HEADER)
    const uint8_t *data;        // TAR_DATA: bytes of the entry's data, inside the input
    size_t data_len;
    const char *error;          // TAR_ERROR: what is wrong (static text)
    uint64_t error_at;          // and the offset of the block where it was found
} tar_t;

// Start reading an archive.
void tar_init(tar_t *t);

// Take input from (*in, *in_len) until something happens, advancing past what was taken. Every
// event but TAR_MORE may leave input untaken: call again with the rest. TAR_DATA points into
// the input, so use it before giving the next piece. After TAR_ERROR, every call returns
// TAR_ERROR. TAR_END takes all the input, and so does every call after it.
tar_event_t tar_feed(tar_t *t, const uint8_t **in, size_t *in_len);

// All the data of the entry last given out has been given out (true at once for one with none).
bool tar_entry_done(const tar_t *t);

// The input has ended. True if the archive ended as it should; false with t->error set
// ("archive ends early", or an earlier error) if not.
bool tar_finish(tar_t *t);

// --- names (§6.3) --------------------------------------------------------------------------------

// The name `name` from an archive, made into the relative path to create on FAT, in `out` (size
// `n`). Leading '/'s are removed (*stripped set, else left alone); '.' and empty components are
// dropped. Returns NULL if the name may be used (out may be "": the target directory itself), or
// why not: "unsafe path" (a ".." component), "name too long" (a component over 255 UTF-16 units,
// or the result over `n`), or "not a valid FAT name" (any other component path_name_check()
// refuses: FAT's forbidden characters, control characters, a trailing space or dot, a '\',
// invalid UTF-8).
const char *tar_name_clean(const char *name, char *out, size_t n, bool *stripped);

// What a refused or skipped entry of kind `kind` (type letter `type`) is said to be, when its
// kind alone makes it skipped: "link skipped (FAT has no links)", "character device skipped",
// ...; "type 'S' skipped" for the others. NULL for files and directories. `buf` (at least 24
// bytes) holds the text for the last case.
const char *tar_kind_skipped(tar_kind_t kind, char type, char *buf, size_t n);

#endif
