// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// src/files/sums_parse.c: a checksum list from anywhere (fetched from a server, say), fed to the
// line splitter in blocks of a size the input chooses, each line parsed for SHA-1, SHA-256 and
// SHA-512, and each name it gives checked and measured as sha256sum would show it.

#include <stdlib.h>
#include <string.h>

#include "files/sums_parse.h"
#include "fuzz.h"

// Checked in every build (NDEBUG or not).
#define MUST(x) do { if (!(x)) abort(); } while (0)

static const struct { const char *tag; size_t size; } algs[] = {
    {"SHA1", 20}, {"SHA256", 32}, {"SHA512", 64},
};

static void check_line(const char *line, size_t len) {
    static char copy[SUMS_LINE_MAX + 1];
    for (size_t a = 0; a < sizeof(algs) / sizeof(algs[0]); a++) {
        memcpy(copy, line, len + 1);
        sums_line_t e;
        sums_line_kind_t k = sums_parse_line(copy, len, algs[a].tag, algs[a].size, &e);
        MUST(k == SUMS_LINE_SKIP || k == SUMS_LINE_OK || k == SUMS_LINE_BAD);
        if (k != SUMS_LINE_OK)
            continue;
        // The name lies inside the line, not empty, and NUL-terminated there.
        MUST(e.name > copy && e.name < copy + len);
        size_t n = strlen(e.name);
        MUST(n > 0 && e.name + n <= copy + len);
        // Shown length: each byte as sums_name_char() has it; flags agree with what it shows.
        size_t shown = 0;
        unsigned flags = 0;
        for (const char *p = e.name; *p; p++) {
            const char *s = sums_name_char(*p);
            shown += s ? strlen(s) : 1;
            if (s && s[0] == '\\')
                flags |= SUMS_NAME_ESCAPED;
            else if (s)
                flags |= SUMS_NAME_CONTROL;
        }
        MUST(shown == sums_name_len(e.name));
        MUST(flags == sums_name_flags(e.name));
        // A safe name is relative and has no "..", ':' or control character.
        if (sums_name_safe(e.name)) {
            MUST(e.name[0] != '/' && e.name[0] != '\\');
            MUST(!strchr(e.name, ':') && !(flags & SUMS_NAME_CONTROL) && !strchr(e.name, '\n'));
            MUST(strcmp(e.name, "..") && strncmp(e.name, "../", 3) && !strstr(e.name, "/../"));
        }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    size_t block = fuzz_byte(&data, &size);
    block = block ? block * 37 : SUMS_LINE_MAX + 1;     // 37 to 9435 bytes, or one buffer
    static sums_lines_t l;
    memset(&l, 0, sizeof(l));
    size_t at = 0;
    bool eof = false;
    unsigned last = 0;
    for (;;) {
        char *line;
        size_t len;
        sums_next_t r = sums_lines_next(&l, eof, &line, &len);
        if (r == SUMS_NEXT_END)
            break;
        if (r == SUMS_NEXT_MORE) {
            MUST(!eof);
            size_t room;
            char *to = sums_lines_room(&l, &room);
            MUST(room >= 1 && to + room <= l.buf + sizeof(l.buf));
            size_t n = size - at < block ? size - at : block;
            n = n < room ? n : room;
            memcpy(to, data + at, n);
            at += n;
            l.len += n;
            eof = n == 0;
            continue;
        }
        MUST(l.line == last + 1);           // numbered in order, each once
        last = l.line;
        if (r == SUMS_NEXT_LINE) {
            MUST(len <= SUMS_LINE_MAX && line >= l.buf && line + len < l.buf + sizeof(l.buf));
            MUST(line[len] == '\0' && !memchr(line, '\n', len));
            check_line(line, len);
        }
    }
    MUST(at == size);                       // the whole input was read
    return 0;
}
