// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/files/gzw.c (gzip's encoder) on zlib built for the host from the firmware's
// sources (cmake/zlib.cmake). What it makes is read back by gz.c (gunzip's decoder) and by GNU
// gunzip when the host has `gzip` (skipped with a note when not): text, an empty file, one byte,
// random data that doesn't compress, data many buffers long; at every level; fed in pieces of
// many sizes and given output room down to one byte, the bytes made are the same. The header is
// `gzip -n`'s (no name, date 0); the heap it takes is within GZW_HEAP and all given back; and
// gzip's output names.

#include <stdlib.h>
#include <unistd.h>

#include "core/heaptag.h"
#include "files/gz.h"
#include "files/gzw.h"
#include "test.h"

typedef struct {
    uint8_t *data;
    size_t len, cap;
} buf_t;

static void put(buf_t *b, const uint8_t *p, size_t n) {
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2 + 64;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

// Encode `in` at `level`, fed in pieces of `piece` bytes into an output buffer of `cap` bytes, as
// gzip does: each piece until it's all taken, then the end until gzw_done(). `bounded` is
// cleared if a call made more than its buffer or stopped with input left and room in `out`.
static buf_t encode(const uint8_t *in, size_t len, int level, size_t piece, size_t cap,
                    bool *bounded) {
    buf_t r = {0};
    uint8_t *out = malloc(cap);
    gzw_t w;
    CHECK(gzw_init(&w, level));
    size_t pos = 0;
    while (!gzw_done(&w)) {
        size_t n = len - pos < piece ? len - pos : piece;
        const uint8_t *p = in + pos;
        size_t left = n;
        bool last = pos + n == len;
        do {
            size_t made = 0;
            CHECK(gzw_feed(&w, &p, &left, last, out, cap, &made));
            if (made > cap || (left && made < cap))
                *bounded = false;
            put(&r, out, made);
            if (made < cap && !left)
                break;
        } while (!gzw_done(&w));
        pos += n;
    }
    CHECK_EQ(w.in_total, len);
    CHECK_EQ(w.out_total, r.len);
    CHECK(heap_tag_peak(HEAP_ZLIB) <= GZW_HEAP);
    gzw_end(&w);
    gzw_end(&w);                            // twice: harmless
    CHECK_EQ(heap_tag_used(HEAP_ZLIB), 0);
    free(out);
    return r;
}

// Decode with gz.c in one piece; true if it gives exactly `want`.
static bool gz_decodes_to(const buf_t *gz, const uint8_t *want, size_t want_len) {
    static uint8_t out[4096];
    gz_t g;
    if (!gz_init(&g))
        return false;
    const uint8_t *p = gz->data;
    size_t left = gz->len, at = 0;
    bool same = true;
    while (same && (left || gz_pending(&g))) {
        size_t made;
        if (!gz_feed(&g, &p, &left, out, sizeof(out), &made))
            same = false;
        else if (at + made > want_len || memcmp(out, want + at, made))
            same = false;
        at += made;
    }
    same = same && gz_finish(&g) && at == want_len && g.members == 1;
    gz_end(&g);
    return same;
}

static bool have_gzip(void) {
    static int have = -1;
    if (have < 0) {
        have = system("gzip --version >/dev/null 2>&1") == 0;
        if (!have)
            printf("note: no gzip on this host: the GNU gunzip checks are skipped\n");
    }
    return have;
}

// Decode with GNU gunzip (`gzip -dc`); true if it accepts the file and gives exactly `want`.
// True too when the host has no gzip (skipped).
static bool gnu_decodes_to(const buf_t *gz, const uint8_t *want, size_t want_len) {
    if (!have_gzip())
        return true;
    char name[] = "/tmp/sos_test_gzw_XXXXXX";
    int fd = mkstemp(name);
    if (fd < 0)
        return false;
    bool ok = write(fd, gz->data, gz->len) == (ssize_t)gz->len;
    close(fd);
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "gzip -dc <%s 2>/dev/null", name);
    FILE *f = ok ? popen(cmd, "r") : NULL;
    buf_t got = {0};
    if (f) {
        uint8_t b[65536];
        size_t n;
        while ((n = fread(b, 1, sizeof(b), f)) > 0)
            put(&got, b, n);
        ok = pclose(f) == 0;
    } else {
        ok = false;
    }
    unlink(name);
    ok = ok && got.len == want_len && (!want_len || !memcmp(got.data, want, want_len));
    free(got.data);
    return ok;
}

// Test data: text (lines of a log), random bytes, and a run of one byte.
static uint8_t *text(size_t len) {
    uint8_t *t = malloc(len + 128);
    size_t at = 0;
    for (unsigned i = 0; at < len; i++)
        at += (size_t)sprintf((char *)t + at, "2026-11-27 12:%02u:%02u wifi: rssi -%u dBm, %u KB free\n",
                              i / 60 % 60, i % 60, 40 + i * 7 % 45, 100 + i * 13 % 200);
    return t;
}

static uint8_t *random_bytes(size_t len) {
    uint8_t *r = malloc(len ? len : 1);
    uint32_t x = 2463534242u;
    for (size_t i = 0; i < len; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        r[i] = (uint8_t)x;
    }
    return r;
}

// Round trip `in` at `level` (one 4 KB piece at a time, gzip's way): gz.c and GNU gunzip give
// it back. Returns the compressed size.
static size_t round_trip(const uint8_t *in, size_t len, int level) {
    bool bounded = true;
    buf_t gz = encode(in, len, level, 4096, 4096, &bounded);
    CHECK(bounded);
    CHECK(gz_decodes_to(&gz, in, len));
    CHECK(gnu_decodes_to(&gz, in, len));
    size_t n = gz.len;
    free(gz.data);
    return n;
}

// --- tests --------------------------------------------------------------------------------------

TEST(every_level_round_trips_text) {
    size_t len = 200000;                    // ~50 buffers
    uint8_t *t = text(len);
    for (int level = 1; level <= 9; level++) {
        size_t n = round_trip(t, len, level);
        CHECK(n < len / 4);
    }
    free(t);
}

TEST(empty_and_one_byte) {
    for (int level = 1; level <= 9; level += 4) {
        CHECK_EQ(round_trip((const uint8_t *)"", 0, level), 20);   // header, empty block, trailer
        round_trip((const uint8_t *)"x", 1, level);
    }
}

TEST(random_data_grows_only_a_little) {
    size_t len = 100000;
    uint8_t *r = random_bytes(len);
    for (int level = 1; level <= 9; level += 4) {
        size_t n = round_trip(r, len, level);
        CHECK(n > len && n <= len + len / 400);     // stored blocks: 0.2 % (5 bytes per ~2 KB)
    }
    free(r);
}

TEST(a_megabyte_of_one_byte) {
    size_t len = 1u << 20;
    uint8_t *z = calloc(1, len);
    size_t n = round_trip(z, len, 6);
    CHECK(n < 8192);
    free(z);
}

TEST(pieces_and_tiny_buffers_give_the_same_bytes) {
    size_t len = 30000;
    uint8_t *t = text(len);
    uint8_t *r = random_bytes(5000);
    memcpy(t + 10000, r, 5000);             // a stored stretch in the middle
    free(r);
    bool bounded = true;
    buf_t whole = encode(t, len, 6, len, 1u << 20, &bounded);
    static const size_t pieces[] = {1, 7, 100, 4096, 30000};
    static const size_t caps[] = {1, 2, 13, 512, 4096};
    for (size_t i = 0; i < sizeof(pieces) / sizeof(pieces[0]); i++)
        for (size_t c = 0; c < sizeof(caps) / sizeof(caps[0]); c++) {
            if (pieces[i] == 1 && caps[c] == 1)
                continue;                   // the same as 1 x 2, ten times slower
            buf_t b = encode(t, len, 6, pieces[i], caps[c], &bounded);
            CHECK(b.len == whole.len && !memcmp(b.data, whole.data, b.len));
            free(b.data);
        }
    CHECK(bounded);
    CHECK(gz_decodes_to(&whole, t, len));
    free(whole.data);
    free(t);
}

TEST(the_header_is_gzip_n) {
    bool bounded = true;
    buf_t gz = encode((const uint8_t *)"hello\n", 6, 6, 6, 4096, &bounded);
    CHECK(gz.len >= 18);
    CHECK_EQ(gz.data[0], 0x1f);
    CHECK_EQ(gz.data[1], 0x8b);
    CHECK_EQ(gz.data[2], 8);                // deflate
    CHECK_EQ(gz.data[3], 0);                // no name, comment or extra
    CHECK_EQ(gz.data[4] | gz.data[5] | gz.data[6] | gz.data[7], 0);    // date 0
    free(gz.data);
}

TEST(wrong_calls_and_levels) {
    gzw_t w;
    CHECK(!gzw_init(&w, 0));
    CHECK(!gzw_init(&w, 10));
    gzw_end(&w);
    CHECK_EQ(heap_tag_used(HEAP_ZLIB), 0);
    CHECK(gzw_init(&w, 1));
    uint8_t out[64];
    size_t made;
    const uint8_t *p = (const uint8_t *)"abc";
    size_t left = 3;
    CHECK(gzw_feed(&w, &p, &left, true, out, sizeof(out), &made));
    CHECK(gzw_done(&w));
    p = (const uint8_t *)"more";
    left = 4;
    CHECK(!gzw_feed(&w, &p, &left, true, out, sizeof(out), &made));    // input after the end
    CHECK(w.error != NULL);
    gzw_end(&w);
    CHECK_EQ(heap_tag_used(HEAP_ZLIB), 0);
}

TEST(output_names) {
    char out[32];
    CHECK(gzw_output_name("/a/notes.txt", out, sizeof(out)));
    CHECK(!strcmp(out, "/a/notes.txt.gz"));
    CHECK(gzw_output_name("x", out, sizeof(out)));
    CHECK(!strcmp(out, "x.gz"));
    CHECK(!gzw_output_name("x.gz", out, sizeof(out)));
    CHECK(!gzw_output_name("/d/X.GZ", out, sizeof(out)));
    CHECK(!gzw_output_name("src.tgz", out, sizeof(out)));
    CHECK(!gzw_output_name("dir/", out, sizeof(out)));
    CHECK(!gzw_output_name("", out, sizeof(out)));
    CHECK(gzw_output_name("/d/.gz", out, sizeof(out)));     // a hidden file named .gz
    CHECK(gzw_output_name("a.gzip", out, sizeof(out)));
    CHECK(!gzw_output_name("a-name-much-too-long-for-32.txt", out, sizeof(out)));
    CHECK(gzw_has_gz_suffix("a.Tgz") && !gzw_has_gz_suffix("a.tar"));
}

TEST_MAIN(
    RUN(every_level_round_trips_text);
    RUN(empty_and_one_byte);
    RUN(random_data_grows_only_a_little);
    RUN(a_megabyte_of_one_byte);
    RUN(pieces_and_tiny_buffers_give_the_same_bytes);
    RUN(the_header_is_gzip_n);
    RUN(wrong_calls_and_levels);
    RUN(output_names);
)
