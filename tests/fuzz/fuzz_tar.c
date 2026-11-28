// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// src/files/tar_parse.c: what `tar tf` and `tar xf` read from an archive, which can hold anything.
// Read once in one piece and once in pieces of a size the first byte chooses, an input gives the
// same entries, the same data (compared by a hash) and the same verdict. Data never comes from
// outside the input, nor more of it than the entry's size. Every name that tar_name_clean()
// accepts is relative, has no "..", "." or empty components, and stays under the target directory
// when joined to it and resolved (§6.3 of docs/design/archives.md). And the input taken as a name
// (up to tar_write.c's limit, no NUL, not ending in '/'), tar_write.c's header for it reads back as that name, as a
// file and as a directory.

#include <stdlib.h>
#include <string.h>

#include "files/path.h"
#include "files/tar_parse.h"
#include "files/tar_write.h"
#include "fuzz.h"

// Checked in every build (NDEBUG or not).
#define MUST(x) do { if (!(x)) abort(); } while (0)

static tar_t t;

static uint64_t mix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

static void check_name(const char *name) {
    static char out[256], full[300], norm[300];
    bool stripped = false;
    if (tar_name_clean(name, out, sizeof(out), &stripped))
        return;
    MUST(out[0] != '/');
    for (const char *c = out; *c;) {
        size_t len = strcspn(c, "/");
        MUST(len > 0);
        MUST(!(len == 1 && c[0] == '.') && !(len == 2 && c[0] == '.' && c[1] == '.'));
        c += len;
        if (*c)
            c++;
    }
    if (!out[0])
        return;
    MUST(path_join("/target", out, full, sizeof(full)));
    MUST(path_resolve("/", full, norm, sizeof(norm)));
    MUST(!strcmp(norm, full) && !strncmp(norm, "/target/", 8));
}

static uint64_t run(const uint8_t *data, size_t size, size_t piece) {
    uint64_t h = 0xcbf29ce484222325ull;
    uint64_t data_left = 0;
    tar_init(&t);
    size_t pos = 0;
    for (;;) {
        size_t n = size - pos < piece ? size - pos : piece;
        const uint8_t *p = data + pos;
        size_t left = n;
        if (!n) {
            bool ok = tar_finish(&t);
            h = mix(h, &ok, sizeof(ok));
            if (!ok)
                h = mix(h, t.error, strlen(t.error));
            return h;
        }
        while (left) {
            tar_event_t ev = tar_feed(&t, &p, &left);   // data hashed as a stream: the pieces
            if (ev == TAR_MORE) {
                MUST(left == 0);
            } else if (ev == TAR_HEADER) {
                MUST(data_left == 0);
                h = mix(h, "header", 6);
                MUST(strlen(t.entry.name) < TAR_NAME_MAX && strlen(t.entry.link) < TAR_NAME_MAX);
                h = mix(h, t.entry.name, strlen(t.entry.name));
                h = mix(h, &t.entry.size, sizeof(t.entry.size));
                h = mix(h, &t.entry.kind, sizeof(t.entry.kind));
                data_left = t.entry.size;
                MUST(tar_entry_done(&t) == (data_left == 0));
                check_name(t.entry.name);
            } else if (ev == TAR_DATA) {
                MUST(t.data >= data && t.data + t.data_len <= data + size);
                MUST(t.data_len > 0 && t.data_len <= data_left);
                data_left -= t.data_len;
                MUST(tar_entry_done(&t) == (data_left == 0));
                h = mix(h, t.data, t.data_len);
            } else if (ev == TAR_END) {
                MUST(left == 0);
                return mix(h, "end", 3);
            } else {
                MUST(t.error);
                return mix(h, t.error, strlen(t.error));
            }
        }
        pos += n;
    }
}

// tar_write.c's headers for `name` read back as it, in one piece.
static void round_trip(const uint8_t *data, size_t size) {
    static char name[TAR_NAME_MAX];
    static uint8_t h[TARW_HEADER_MAX];
    if (!size || size > TAR_NAME_MAX - 2 || memchr(data, '\0', size) || data[size - 1] == '/') {
        if (size && size < sizeof(name)) {
            memcpy(name, data, size);
            name[size] = '\0';
            MUST(strlen(name) < size || tarw_header(h, name, false, 0, 0) == 0);
        }
        return;
    }
    memcpy(name, data, size);
    name[size] = '\0';
    for (int dir = 0; dir < 2; dir++) {
        size_t len = tarw_header(h, name, dir, 1234, 1772600767);
        MUST(len > 0 && len <= sizeof(h) && len % TAR_BLOCK == 0);
        tar_init(&t);
        const uint8_t *p = h;
        size_t left = len;
        MUST(tar_feed(&t, &p, &left) == TAR_HEADER && left == 0);
        MUST(!t.entry.refused && !strncmp(t.entry.name, name, size));
        MUST(!strcmp(t.entry.name + size, dir ? "/" : ""));
        MUST(t.entry.kind == (dir ? TAR_DIR : TAR_FILE) && t.entry.mtime == 1772600767);
        MUST(t.entry.size == (dir ? 0 : 1234));
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // [piece - 1] then the archive.
    size_t piece = (size_t)fuzz_byte(&data, &size) * 3 + 1;
    uint64_t a = run(data, size, size ? size : 1);
    uint64_t b = run(data, size, piece);
    MUST(a == b);
    round_trip(data, size);
    return 0;
}
