// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/files/tar_write.c: the headers `tar cf` writes, read back by tar_parse.c in
// one piece and block by block, and listed and extracted by GNU tar and bsdtar when the host has
// them (tests/tar_tools.h): short names, names at the 100-byte limit, names split into ustar's
// prefix, names only a pax record holds (to the longest tar_parse.c reads), UTF-8 cut at a
// character, directories, empty files, sizes in base-256, dates, and the end of the archive
// padded to GNU's 10240-byte record.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "files/tar_parse.h"
#include "files/tar_write.h"
#include "tar_tools.h"
#include "test.h"

typedef struct {
    uint8_t *data;
    size_t len, cap;
} buf_t;

static void put(buf_t *b, const void *p, size_t n) {
    while (b->len + n > b->cap)
        b->data = realloc(b->data, b->cap = b->cap ? 2 * b->cap : 65536);
    if (n)
        memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void zeros(buf_t *b, size_t n) {
    static const uint8_t z[TARW_RECORD];
    while (n) {
        size_t k = n < sizeof(z) ? n : sizeof(z);
        put(b, z, k);
        n -= k;
    }
}

// The archive of n entries, as tar.c writes it: header, data, padding; the end.
static void archive(buf_t *b, const tt_entry_t *e, size_t n) {
    b->len = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t h[TARW_HEADER_MAX];
        size_t hl = tarw_header(h, e[i].name, e[i].dir, e[i].len, e[i].mtime);
        CHECK(hl > 0 && hl % TAR_BLOCK == 0);
        put(b, h, hl);
        if (!e[i].dir) {
            put(b, e[i].data, e[i].len);
            zeros(b, tarw_pad(e[i].len));
        }
    }
    zeros(b, (size_t)tarw_end(b->len));
    CHECK_EQ(b->len % TARW_RECORD, 0);
}

// tar_parse.c reads the archive in pieces of `piece` bytes as exactly the entries e.
static bool parses_as(const buf_t *b, size_t piece, const tt_entry_t *e, size_t n) {
    static tar_t t;
    tar_init(&t);
    size_t i = 0, got = 0;
    bool ok = true, ended = false;
    for (size_t pos = 0; pos < b->len && ok && !ended; pos += piece) {
        const uint8_t *p = b->data + pos;
        size_t left = b->len - pos < piece ? b->len - pos : piece;
        while (left && ok && !ended) {
            switch (tar_feed(&t, &p, &left)) {
            case TAR_MORE:
                break;
            case TAR_HEADER:
                ok = i < n && !t.entry.refused && t.entry.kind == (e[i].dir ? TAR_DIR : TAR_FILE);
                if (ok) {
                    size_t l = strlen(e[i].name);
                    ok = !strncmp(t.entry.name, e[i].name, l) &&
                         !strcmp(t.entry.name + l, e[i].dir ? "/" : "") &&
                         t.entry.size == (e[i].dir ? 0 : e[i].len) &&
                         t.entry.mtime == e[i].mtime;
                    if (!ok)
                        fprintf(stderr, "  read \"%s\" (%llu bytes, %lld)\n", t.entry.name,
                                (unsigned long long)t.entry.size, (long long)t.entry.mtime);
                }
                got = 0;
                i++;
                break;
            case TAR_DATA:
                ok = got + t.data_len <= e[i - 1].len &&
                     !memcmp(t.data, e[i - 1].data + got, t.data_len);
                got += t.data_len;
                break;
            case TAR_END:
                ended = true;
                break;
            case TAR_ERROR:
                fprintf(stderr, "  %s at %llu\n", t.error, (unsigned long long)t.error_at);
                ok = false;
                break;
            }
        }
    }
    return ok && ended && i == n;
}

static void check_all(const tt_entry_t *e, size_t n) {
    static buf_t b;
    archive(&b, e, n);
    CHECK(parses_as(&b, b.len, e, n));
    CHECK(parses_as(&b, TAR_BLOCK, e, n));
    CHECK(parses_as(&b, 7, e, n));
    CHECK(tt_check("tar", b.data, b.len, e, n));
    CHECK(tt_check("bsdtar", b.data, b.len, e, n));
}

static char *repeat(char *p, char c, size_t n) {
    memset(p, c, n);
    return p + n;
}

#define T0 1772600767       // 2026-03-04 05:06:07 UTC

TEST(short_names_dirs_empty_files) {
    static const uint8_t hello[] = "hello\n";
    const tt_entry_t e[] = {
        {"a", true, NULL, 0, T0},
        {"a/hello.txt", false, hello, 6, T0 + 1},
        {"a/empty", false, (const uint8_t *)"", 0, T0},
        {"a/b", true, NULL, 0, T0},
        {"a/b/c", true, NULL, 0, T0},
        {"top.bin", false, hello, 5, 315532800},    // 1980-01-01
        {"caf\xc3\xa9.txt", false, hello, 6, T0},
    };
    check_all(e, sizeof(e) / sizeof(e[0]));
}

TEST(names_at_the_limits) {
    static char n100[101], d99[100], split[300], pax_one[300], pax_dir[300], pax_deep[800];
    static char longest[800];
    static char utf8[300];
    memset(n100, 'n', 100);                         // exactly the name field
    memset(d99, 'd', 99);                           // with its '/', exactly the field too
    char *p = repeat(split, 'p', 150);              // prefix 150, name 90
    *p++ = '/';
    repeat(p, 'q', 90);
    p = repeat(pax_one, 'o', 200);                  // one component: no '/' to split at
    *p = '\0';
    memset(pax_dir, 'O', 180);
    p = pax_deep;                                   // 16 levels of 40 bytes: 655 bytes
    for (int i = 0; i < 16; i++) {
        p = repeat(p, (char)('a' + i), 40);
        *p++ = '/';
    }
    strcpy(p, "file.txt");
    p = longest;                                    // TAR_NAME_MAX - 2: 764 bytes
    for (int i = 0; i < 3; i++) {
        p = repeat(p, (char)('A' + i), 250);
        *p++ = '/';
    }
    repeat(p, 'Z', TAR_NAME_MAX - 2 - 753);
    p = utf8;                                       // 2-byte characters across byte 100
    for (int i = 0; i < 60; i++) {
        *p++ = '\xc3';
        *p++ = '\xa9';
    }
    static const uint8_t data[] = "x";
    const tt_entry_t e[] = {
        {n100, false, data, 1, T0},
        {d99, true, NULL, 0, T0},
        {split, false, data, 1, T0},
        {pax_one, false, data, 1, T0},
        {pax_dir, true, NULL, 0, T0},
        {pax_deep, false, data, 1, T0},
        {utf8, false, data, 1, T0},
    };
    check_all(e, sizeof(e) / sizeof(e[0]));
    // The longest name: our reader takes it; the host tools' file systems may not (255-byte
    // components only), so they're not asked.
    static buf_t b;
    const tt_entry_t big[] = {{longest, false, data, 1, T0}};
    archive(&b, big, 1);
    CHECK(parses_as(&b, 1, big, 1));
    uint8_t h[TARW_HEADER_MAX];
    CHECK_EQ(tarw_header(h, longest, false, 1, T0), TARW_HEADER_MAX);
    // One byte more, or none: refused.
    static char over[800];
    memset(over, 'v', TAR_NAME_MAX - 1);
    CHECK_EQ(tarw_header(h, over, false, 1, T0), 0);
    CHECK_EQ(tarw_header(h, "", true, 0, T0), 0);
    CHECK_EQ(tarw_header(h, "a/", true, 0, T0), 0);
    // A split puts the prefix and the name in their fields.
    CHECK_EQ(tarw_header(h, split, false, 1, T0), TAR_BLOCK);
    CHECK(h[345] == 'p' && h[345 + 149] == 'p' && h[345 + 150] == '\0' && h[0] == 'q');
}

TEST(numbers_and_checksum) {
    uint8_t h[TARW_HEADER_MAX];
    CHECK_EQ(tarw_header(h, "f", false, 01234567, T0), TAR_BLOCK);
    CHECK(!memcmp(h + 100, "0000644\0" "0000000\0" "0000000\0" "00001234567\0", 36));
    CHECK(!memcmp(h + 156, "0", 1) && !memcmp(h + 257, "ustar\0" "00", 8));
    CHECK(h[154] == '\0' && h[155] == ' ');
    CHECK_EQ(tarw_header(h, "d", true, 99, -5), TAR_BLOCK);
    CHECK(!memcmp(h + 100, "0000755", 7) && !memcmp(h + 124, "00000000000\0" "00000000000\0", 24));
    CHECK(h[156] == '5' && !strcmp((char *)h, "d/"));
    // 8 GB and over: base-256. tar_parse.c reads it back.
    uint64_t big = 9ull << 30;
    CHECK_EQ(tarw_header(h, "huge", false, big, T0), TAR_BLOCK);
    CHECK(h[124] == 0x80);
    static tar_t t;
    tar_init(&t);
    const uint8_t *p = h;
    size_t left = TAR_BLOCK;
    CHECK_EQ(tar_feed(&t, &p, &left), TAR_HEADER);
    CHECK(t.entry.size == big && t.entry.mtime == T0);
    CHECK_EQ(tarw_header(h, "edge", false, 077777777777ull, T0), TAR_BLOCK);
    CHECK(h[124] == '7');
}

TEST(padding_and_end) {
    CHECK_EQ(tarw_pad(0), 0);
    CHECK_EQ(tarw_pad(1), 511);
    CHECK_EQ(tarw_pad(512), 0);
    CHECK_EQ(tarw_pad(513), 511);
    CHECK_EQ(tarw_end(0), TARW_RECORD);
    CHECK_EQ(tarw_end(512), TARW_RECORD - 512);
    CHECK_EQ(tarw_end(TARW_RECORD - 1024), 1024);
    CHECK_EQ(tarw_end(TARW_RECORD - 512), 512 + TARW_RECORD);
    CHECK_EQ(tarw_end(TARW_RECORD), TARW_RECORD);
}

TEST_MAIN(
    RUN(short_names_dirs_empty_files);
    RUN(names_at_the_limits);
    RUN(numbers_and_checksum);
    RUN(padding_and_end);
)
