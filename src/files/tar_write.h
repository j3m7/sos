// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The writing side of tar (docs/design/archives.md §6.7): the header blocks for a file or a
// directory, and the zeros that end an archive. Pure C (no FatFs, no allocation), host tested
// (tests/test_tar_write.c: read back by files/tar_parse.h, and by GNU tar and bsdtar when the
// host has them); files/tar.c writes what it makes, then the file's data padded to a block.
//
// What it writes: POSIX ustar headers (magic "ustar\0", version "00"), mode 0644 for files and
// 0755 for directories, uid and gid 0, no user or group names. A name that fits the 100-byte
// name field, or splits at a '/' into the 155-byte prefix and the name field, goes there;
// a longer one gets a pax 'x' record (`path`) before its header, as GNU tar and bsdtar write it.
// Directories are stored with a trailing '/'. Sizes and dates in octal, sizes from 8 GB in
// GNU's base-256 (GNU tar, bsdtar and tar_parse.c read it).

#ifndef SOS_TAR_WRITE_H
#define SOS_TAR_WRITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "files/tar_parse.h"

// The most tarw_header() writes: a pax header, its record (two blocks for the longest name),
// and the entry's header.
#define TARW_HEADER_MAX (4 * TAR_BLOCK)

// GNU tar's record: an archive's length is rounded up to this with zeros after the end blocks.
#define TARW_RECORD (20 * TAR_BLOCK)

// The header blocks for `name` (relative, no leading '/'; UTF-8; a directory's without its
// trailing '/', which is added) into out (at least TARW_HEADER_MAX bytes): a file of `size`
// bytes, or a directory (`dir`, size ignored), dated `mtime` (seconds since 1970, UTC; dates
// before 1970 are written as 0). Returns the bytes written, a multiple of TAR_BLOCK, or 0 if
// `name` is empty, ends in '/', or is longer than TAR_NAME_MAX - 2 bytes (what tar_parse.c reads
// back, with the '/'). The file's data follows, then zeros to the next block (tarw_pad()).
size_t tarw_header(uint8_t *out, const char *name, bool dir, uint64_t size, int64_t mtime);

// Zeros after `size` bytes of data, up to the next block: 0 to 511.
size_t tarw_pad(uint64_t size);

// The zeros that end an archive of `written` bytes (a multiple of TAR_BLOCK): two zero blocks,
// then up to the next TARW_RECORD, as GNU tar ends one.
uint64_t tarw_end(uint64_t written);

#endif
