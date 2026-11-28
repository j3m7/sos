// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The text side of sha1sum, sha256sum and sha512sum (docs/design/archives.md §4): how a name is
// shown, a checksum list split into lines and each line parsed, and which names in a list may be
// read. Pure C (no FatFs, no wolfCrypt), so it's unit tested and fuzzed on the host.

#ifndef SOS_SUMS_PARSE_H
#define SOS_SUMS_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SUMS_LINE_MAX   4096    // a list's line at most, without its '\n' (a '\r' counts)
#define SUMS_DIGEST_MAX 64      // SHA-512

// --- names as printed --------------------------------------------------------------------------

// GNU's rule: a name holding '\n' or '\' is printed escaped ("\n", "\\") and its line marked
// with a leading '\'. SOS adds: other control characters (escape sequences) never reach the
// terminal; they are shown as '?', and a hash line isn't printed for such a name.
#define SUMS_NAME_ESCAPED 1u    // has '\n' or '\': escaped, the line marked
#define SUMS_NAME_CONTROL 2u    // has another control character (< 0x20, 0x7f)

unsigned sums_name_flags(const char *name);

// How the byte `c` of a name is shown: NULL for itself, else the text standing for it ("\\n",
// "\\\\" or "?").
const char *sums_name_char(char c);

// The length of `name` as shown (each byte as sums_name_char() has it).
size_t sums_name_len(const char *name);

// --- a list, line by line ----------------------------------------------------------------------

// The list is read in blocks into `buf` and split at '\n'. A line longer than SUMS_LINE_MAX is
// dropped up to its end and reported once (SUMS_NEXT_LONG). Zero the struct to start.
typedef struct {
    char buf[SUMS_LINE_MAX + 1];
    size_t len;                 // bytes in buf
    size_t used;                // bytes at its start already returned as lines
    bool skipping;              // inside a line too long to keep: drop up to its '\n'
    unsigned line;              // the number of the line last returned (from 1)
} sums_lines_t;

typedef enum {
    SUMS_NEXT_LINE,             // a line: *line, *len, NUL-terminated at (*line)[*len]
    SUMS_NEXT_LONG,             // line number l->line was too long, and is skipped
    SUMS_NEXT_MORE,             // read more into sums_lines_room(), or say the list ended
    SUMS_NEXT_END,              // the list ended (after `eof`), nothing left
} sums_next_t;

// The next line. `eof`: the list has no more bytes. A line returned stays valid (and may be
// changed in place, sums_parse_line()) until the next call to sums_lines_next() or
// sums_lines_room().
sums_next_t sums_lines_next(sums_lines_t *l, bool eof, char **line, size_t *len);

// After SUMS_NEXT_MORE: where to read the next bytes, and how many fit (at least 1). Then add
// what was read to l->len.
char *sums_lines_room(sums_lines_t *l, size_t *room);

// --- a line ------------------------------------------------------------------------------------

typedef enum {
    SUMS_LINE_SKIP,             // blank, or a comment ('#')
    SUMS_LINE_OK,               // a checksum and a name
    SUMS_LINE_BAD,              // improperly formatted
} sums_line_kind_t;

typedef struct {
    uint8_t digest[SUMS_DIGEST_MAX];
    char *name;                 // inside the line, unescaped, NUL-terminated, not empty
} sums_line_t;

// Parse one line of a list (without its '\n'; a trailing '\r' is dropped) for the algorithm
// whose BSD tag is `tag` ("SHA256") and whose digest is `size` bytes. Forms (§4.2):
//   <hex>  <name>          GNU, text mode
//   <hex> *<name>          GNU, binary mode (the same on SOS)
//   <tag> (<name>) = <hex> BSD; another algorithm's tag is improperly formatted
// either with a leading '\': the name is escaped ("\\" and "\n", nothing else). Hex in either
// case, exactly `size` bytes of it. A line holding a NUL byte is improperly formatted. The line
// is changed in place, and line[len] must be writable (sums_lines_next() lines are).
sums_line_kind_t sums_parse_line(char *line, size_t len, const char *tag, size_t size,
                                 sums_line_t *out);

// May a list name this file? Names are relative to the list's directory: refused are names
// that are absolute ('/' or '\' first), that have a ".." component (split at '/' and '\', as
// FatFs splits; also a component of dots and spaces only with two or more dots, which FatFs may
// take as ".."), and names with ':' (a drive prefix) or a control character (FatFs ends the
// path there).
bool sums_name_safe(const char *name);

#endif
