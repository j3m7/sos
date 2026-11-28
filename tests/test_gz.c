// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/files/gz.c (gunzip's and zcat's decoder) on zlib built for the host from
// the firmware's sources (cmake/zlib.cmake), over files made by GNU gzip
// (tests/data/archives/, tools/make-archive-fixtures.sh): one member and several, a header with
// a name, stored blocks, trailing zeros, trailing garbage, a bad CRC, a bad length, damaged
// data, a cut end, an empty file, not gzip at all. Each is fed in pieces of every size (or many
// sizes, for the long ones), and decodes the same whatever the pieces and the output buffer;
// no call makes more output than its buffer holds. And gunzip's output names.

#include <stdlib.h>

#include "core/heaptag.h"
#include "files/gz.h"
#include "test.h"

#ifndef ARCHIVES_DIR
#error "ARCHIVES_DIR: tests/data/archives"
#endif

typedef struct {
    uint8_t *data;
    size_t len;
} blob_t;

static blob_t load(const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", ARCHIVES_DIR, name);
    blob_t b = {NULL, 0};
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "%s: can't open\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    b.len = (size_t)size;
    b.data = malloc(b.len ? b.len : 1);
    if (!b.data || fread(b.data, 1, b.len, f) != b.len) {
        fprintf(stderr, "%s: can't read\n", path);
        exit(1);
    }
    fclose(f);
    return b;
}

// What a decode gave.
typedef struct {
    bool ok;
    const char *error;
    bool zeros;
    unsigned members;
    uint8_t *out;
    size_t out_len;
    bool bounded;               // no call made more than its buffer, and each stopped for a reason
} result_t;

// Decode `in` fed in pieces of `piece` bytes (or, with `cut`, in two: before and from `cut`),
// into an output buffer of `cap` bytes, as gunzip does: feed each piece until it's all taken,
// drain what zlib holds at the end, then finish.
static result_t decode_cut(const blob_t *in, size_t piece, size_t cap, size_t cut) {
    result_t r = {.bounded = true};
    uint8_t *buf = malloc(cap);
    size_t out_cap = 4096;
    r.out = malloc(out_cap);
    gz_t g;
    CHECK(gz_init(&g));
    size_t pos = 0;
    bool failed = false;
    while (!failed) {
        size_t n = in->len - pos < piece ? in->len - pos : piece;
        if (cut)
            n = pos < cut ? cut - pos : in->len - pos;
        const uint8_t *p = in->data + pos;
        size_t left = n;
        if (!n && !gz_pending(&g))
            break;
        do {
            size_t made = 0;
            bool fed = gz_feed(&g, &p, &left, buf, cap, &made);
            if (made > cap)
                r.bounded = false;
            if (fed && left && made != cap)     // stopped with input left: only when full
                r.bounded = false;
            while (r.out_len + made > out_cap)
                r.out = realloc(r.out, out_cap *= 2);
            memcpy(r.out + r.out_len, buf, made);
            r.out_len += made;
            if (!fed)
                failed = true;
        } while (!failed && (left || gz_pending(&g)));
        pos += n - left;
    }
    r.ok = !failed && gz_finish(&g);
    r.error = r.ok ? NULL : g.error;
    r.zeros = g.trailing_zeros;
    r.members = g.members;
    CHECK_EQ(g.out_total, r.out_len);
    gz_end(&g);
    gz_end(&g);                                 // twice is harmless
    free(buf);
    return r;
}

static result_t decode(const blob_t *in, size_t piece, size_t cap) {
    return decode_cut(in, piece, cap, 0);
}

typedef struct {
    const char *file;
    const char *expect;         // the file it decodes to, or NULL for an error
    const char *error;          // the error, or "" for any
    bool zeros;
    unsigned members;
} fixture_t;

static const fixture_t fixtures[] = {
    {"text.txt.gz", "text.txt", NULL, false, 1},
    {"name.txt.gz", "text.txt", NULL, false, 1},
    {"multi.gz", "text.txt", NULL, false, 3},
    {"zeros.gz", "text.txt", NULL, true, 1},
    {"random.bin.gz", "random.bin", NULL, false, 1},
    {"emptydata.gz", NULL, NULL, false, 1},     // decodes to nothing (checked below)
    {"junk.gz", NULL, "trailing garbage after the data", false, 1},
    {"zerosjunk.gz", NULL, "trailing garbage after the data", true, 1},
    {"badmember.gz", NULL, "unknown compression method", false, 1},
    {"badcrc.gz", NULL, "CRC error: the data is corrupt", false, 0},
    {"badlen.gz", NULL, "length error: the data is corrupt", false, 0},
    {"baddata.gz", NULL, "", false, 0},
    {"truncated.gz", NULL, "unexpected end of file", false, 0},
    {"truncated-half.gz", NULL, "unexpected end of file", false, 0},
    {"empty.gz", NULL, "empty file", false, 0},
    {"notgz.gz", NULL, "not in gzip format", false, 0},
};

// One fixture, fed in pieces of `piece` into a `cap`-byte buffer, gives what the table says.
static bool check_fixture_cut(const fixture_t *f, const blob_t *in, const blob_t *want,
                              size_t piece, size_t cap, size_t cut) {
    result_t r = decode_cut(in, piece, cap, cut);
    bool good = r.bounded && r.zeros == f->zeros && r.members == f->members;
    bool success = f->expect || !strcmp(f->file, "emptydata.gz");
    if (success)
        good = good && r.ok && r.out_len == want->len && !memcmp(r.out, want->data, want->len);
    else
        good = good && !r.ok && r.error && (!*f->error || !strcmp(r.error, f->error));
    // An error after a whole member gives that member's data first.
    if (!success && f->members == 1 && strcmp(f->file, "emptydata.gz"))
        good = good && r.out_len == want->len && !memcmp(r.out, want->data, want->len);
    if (!good)
        fprintf(stderr, "  %s, pieces of %zu (cut at %zu), buffer %zu: ok %d error \"%s\" zeros %d "
                "members %u output %zu bytes\n", f->file, piece, cut, cap, r.ok,
                r.error ? r.error : "", r.zeros, r.members, r.out_len);
    free(r.out);
    return good;
}

static bool check_fixture(const fixture_t *f, const blob_t *in, const blob_t *want, size_t piece,
                          size_t cap) {
    return check_fixture_cut(f, in, want, piece, cap, 0);
}

static blob_t expected(const fixture_t *f) {
    if (f->expect)
        return load(f->expect);
    if (f->members == 1 && strcmp(f->file, "emptydata.gz"))
        return load("text.txt");                // the member before the garbage
    return (blob_t){calloc(1, 1), 0};
}

TEST(each_fixture_decodes_as_gnu_gzip_says_in_one_piece) {
    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
        blob_t in = load(fixtures[i].file), want = expected(&fixtures[i]);
        CHECK(check_fixture(&fixtures[i], &in, &want, in.len ? in.len : 1, 4096));
        free(in.data);
        free(want.data);
    }
}

// Pieces of every size from 1 to 128 bytes, then of sizes 97 apart up to the whole file, into
// gunzip's 4 KB buffer. (Every size up to the whole file, about 13000 decodes a file, takes a
// minute and a half; it passed when this was written.)
TEST(every_piece_size_decodes_the_same) {
    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
        blob_t in = load(fixtures[i].file), want = expected(&fixtures[i]);
        unsigned bad = 0;
        for (size_t piece = 1; piece <= in.len && bad <= 3; piece += piece < 128 ? 1 : 97)
            bad += !check_fixture(&fixtures[i], &in, &want, piece, 4096);
        CHECK_EQ(bad, 0);
        free(in.data);
        free(want.data);
    }
}

// Two pieces, cut at every place in the first and last 200 bytes: in the headers, the trailers
// and what follows them.
TEST(every_cut_near_the_ends_decodes_the_same) {
    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
        blob_t in = load(fixtures[i].file), want = expected(&fixtures[i]);
        unsigned bad = 0;
        for (size_t at = 1; at < in.len && bad <= 3;
             at = at == 200 && in.len > 400 ? in.len - 200 : at + 1)
            bad += !check_fixture_cut(&fixtures[i], &in, &want, in.len, 4096, at);
        CHECK_EQ(bad, 0);
        free(in.data);
        free(want.data);
    }
}

// Small output buffers (zcat on a slow terminal gives less than 4 KB), with small pieces too.
TEST(small_output_buffers_decode_the_same) {
    static const size_t caps[] = {1, 2, 3, 7, 64, 511, 4095};
    static const size_t pieces[] = {1, 2, 5, 13, 4096, 1 << 20};
    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
        blob_t in = load(fixtures[i].file), want = expected(&fixtures[i]);
        for (size_t c = 0; c < sizeof(caps) / sizeof(caps[0]); c++)
            for (size_t p = 0; p < sizeof(pieces) / sizeof(pieces[0]); p++) {
                if (caps[c] < 64 && pieces[p] < 13 && in.len > 4096)
                    continue;                   // millions of calls: covered by the others
                CHECK(check_fixture(&fixtures[i], &in, &want, pieces[p], caps[c]));
            }
        free(in.data);
        free(want.data);
    }
}

// 1 MB of zeros from about 1 KB: each call still makes at most its buffer, many calls per byte
// of input; and zlib's memory stays within GZ_HEAP and is all given back.
TEST(a_small_bomb_is_decoded_a_buffer_at_a_time) {
    blob_t in = load("zeros1m.gz");
    static const size_t pieces[] = {1, 7, 100, 4096};
    for (size_t p = 0; p < sizeof(pieces) / sizeof(pieces[0]); p++) {
        result_t r = decode(&in, pieces[p], 4096);
        CHECK(r.ok && r.bounded);
        CHECK_EQ(r.out_len, 1048576);
        bool zero = true;
        for (size_t k = 0; k < r.out_len; k++)
            zero = zero && r.out[k] == 0;
        CHECK(zero);
        free(r.out);
    }
    CHECK_EQ(heap_tag_used(HEAP_ZLIB), 0);
    CHECK(heap_tag_peak(HEAP_ZLIB) > 32768);    // the window
    CHECK(heap_tag_peak(HEAP_ZLIB) <= GZ_HEAP);
    free(in.data);
}

TEST(after_an_error_nothing_more_is_decoded) {
    blob_t in = load("notgz.gz");
    gz_t g;
    CHECK(gz_init(&g));
    uint8_t buf[64];
    size_t made, left = in.len;
    const uint8_t *p = in.data;
    CHECK(!gz_feed(&g, &p, &left, buf, sizeof(buf), &made));
    CHECK_EQ(made, 0);
    CHECK(!strcmp(g.error, "not in gzip format"));
    blob_t ok = load("text.txt.gz");
    p = ok.data;
    left = ok.len;
    CHECK(!gz_feed(&g, &p, &left, buf, sizeof(buf), &made));
    CHECK_EQ(made, 0);
    CHECK_EQ(left, ok.len);
    CHECK(!gz_finish(&g));
    CHECK(!strcmp(g.error, "not in gzip format"));
    gz_end(&g);
    free(in.data);
    free(ok.data);
}

// A member's magic bytes split across calls, and a lone 1f at the end (a cut member).
TEST(magic_split_across_calls_and_a_cut_magic) {
    blob_t in = load("text.txt.gz"), want = load("text.txt");
    gz_t g;
    CHECK(gz_init(&g));
    uint8_t *out = malloc(want.len + 1);
    size_t total = 0, made, left = 1;
    const uint8_t *p = in.data;
    CHECK(gz_feed(&g, &p, &left, out, want.len + 1, &made));
    CHECK_EQ(made, 0);
    CHECK_EQ(left, 0);
    left = in.len - 1;
    CHECK(gz_feed(&g, &p, &left, out, want.len + 1, &made));
    total += made;
    CHECK_EQ(total, want.len);
    CHECK(!memcmp(out, want.data, want.len));
    static const uint8_t lone = 0x1f;
    p = &lone;
    left = 1;
    CHECK(gz_feed(&g, &p, &left, out, want.len + 1, &made));
    CHECK(!gz_finish(&g));
    CHECK(!strcmp(g.error, "unexpected end of file"));
    gz_end(&g);
    free(out);
    free(in.data);
    free(want.data);
}

TEST(output_names_follow_gnu_gunzip) {
    char out[16];
    CHECK(gz_output_name("a.gz", out, sizeof(out)) && !strcmp(out, "a"));
    CHECK(gz_output_name("/d/pkg-1.2.tar.gz", out, sizeof(out)) && !strcmp(out, "/d/pkg-1.2.tar"));
    CHECK(gz_output_name("/d/pkg.tgz", out, sizeof(out)) && !strcmp(out, "/d/pkg.tar"));
    CHECK(gz_output_name("LOG.GZ", out, sizeof(out)) && !strcmp(out, "LOG"));
    CHECK(gz_output_name("x.TGZ", out, sizeof(out)) && !strcmp(out, "x.tar"));
    CHECK(gz_output_name("x.gz.gz", out, sizeof(out)) && !strcmp(out, "x.gz"));
    CHECK(!gz_output_name("x.zip", out, sizeof(out)));
    CHECK(!gz_output_name("x.gzip", out, sizeof(out)));
    CHECK(!gz_output_name("xgz", out, sizeof(out)));
    CHECK(!gz_output_name(".gz", out, sizeof(out)));
    CHECK(!gz_output_name("/d/.gz", out, sizeof(out)));
    CHECK(!gz_output_name("/d/.tgz", out, sizeof(out)));
    CHECK(!gz_output_name("x", out, sizeof(out)));
    CHECK(!gz_output_name("", out, sizeof(out)));
    CHECK(gz_output_name("123456789012345.gz", out, sizeof(out)) &&
          !strcmp(out, "123456789012345"));
    CHECK(!gz_output_name("1234567890123456.gz", out, sizeof(out)));     // doesn't fit
    CHECK(!gz_output_name("123456789012.tgz", out, sizeof(out)));        // .tar doesn't fit
}

TEST_MAIN(
    RUN(each_fixture_decodes_as_gnu_gzip_says_in_one_piece);
    RUN(every_piece_size_decodes_the_same);
    RUN(every_cut_near_the_ends_decodes_the_same);
    RUN(small_output_buffers_decode_the_same);
    RUN(a_small_bomb_is_decoded_a_buffer_at_a_time);
    RUN(after_an_error_nothing_more_is_decoded);
    RUN(magic_split_across_calls_and_a_cut_magic);
    RUN(output_names_follow_gnu_gunzip);
)
