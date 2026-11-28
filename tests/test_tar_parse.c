// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/files/tar_parse.c over archives made by GNU tar, bsdtar and python's tarfile
// (tests/data/archives/tar/, tools/make-archive-fixtures.sh): ustar with its prefix, GNU long
// names ('L'), pax long names, linkpath and global headers, v7, base-256 sizes and dates, a
// signed checksum, links, devices, a FIFO; names and pax records too long; a bad checksum, an
// archive cut short or without its end blocks. Each is fed whole and in pieces of many sizes
// (every size up to 600 for the small ones) and must read the same. And the name rules (§6.3).

#include <stdlib.h>

#include "files/tar_parse.h"
#include "test.h"

#ifndef ARCHIVES_DIR
#error "ARCHIVES_DIR: tests/data/archives"
#endif

#define MTIME 1772600767                // 2026-03-04 05:06:07 UTC, every fixture's date

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
    b.len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    b.data = malloc(b.len ? b.len : 1);
    if (!b.data || fread(b.data, 1, b.len, f) != b.len)
        exit(1);
    fclose(f);
    return b;
}

static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
#define FNV0 0xcbf29ce484222325ull

static tar_t t;
static char log_buf[1 << 16];

// Read `b` in pieces of `piece` bytes; what was read, one line per entry, into log_buf:
// "<kind> <size> <mtime> <name>[ -> <link>][ !<refused>] #<hash of the data>", then "end",
// or "error: <why> at <offset>".
static const char *read_all(blob_t b, size_t piece) {
    static const char kinds[] = "-dhlcbp?";
    size_t used = 0, pos = 0;
    uint64_t h = FNV0;
    bool in_entry = false, ended = false;
    log_buf[0] = '\0';
    tar_init(&t);
#define LOG(...) used += (size_t)snprintf(log_buf + used, sizeof(log_buf) - used, __VA_ARGS__)
#define CLOSE() do { if (in_entry) LOG(" #%016llx\n", (unsigned long long)h); in_entry = false; } while (0)
    while (!ended) {
        size_t n = b.len - pos < piece ? b.len - pos : piece;
        const uint8_t *p = b.data + pos;
        size_t left = n;
        if (!n) {
            CLOSE();
            if (tar_finish(&t))
                LOG("end\n");
            else
                LOG("error: %s at %llu\n", t.error, (unsigned long long)t.error_at);
            break;
        }
        while (left || !n) {
            tar_event_t ev = tar_feed(&t, &p, &left);
            if (ev == TAR_MORE) {
                CHECK_EQ(left, 0);
                break;
            }
            if (ev == TAR_HEADER) {
                CLOSE();
                tar_entry_t *e = &t.entry;
                LOG("%c %llu %lld %s", kinds[e->kind], (unsigned long long)e->size,
                    (long long)e->mtime, e->name);
                if (e->link[0])
                    LOG(" -> %s", e->link);
                if (e->refused)
                    LOG(" !%s", e->refused);
                h = FNV0;
                in_entry = true;
            } else if (ev == TAR_DATA) {
                CHECK(t.data_len > 0 && t.data_len <= n);
                h = fnv(h, t.data, t.data_len);
            } else if (ev == TAR_END) {
                CHECK_EQ(left, 0);
                CLOSE();
                LOG("end\n");
                ended = true;
                break;
            } else {
                CLOSE();
                LOG("error: %s at %llu\n", t.error, (unsigned long long)t.error_at);
                CHECK(tar_feed(&t, &p, &left) == TAR_ERROR);
                ended = true;
                break;
            }
        }
        pos += n;
    }
    return log_buf;
#undef LOG
#undef CLOSE
}

// Read whole, then in many piece sizes: all the same. Returns the log (a copy).
static char *read_every_way(const char *name) {
    blob_t b = load(name);
    char *whole = strdup(read_all(b, b.len ? b.len : 1));
    size_t max = b.len < 12000 ? 600 : 70;
    for (size_t piece = 1; piece <= max; piece++) {
        size_t sz = piece <= 70 ? piece : piece * 7;   // past 70: 7-byte steps up to 4200
        if (sz > 600 && b.len >= 12000)
            break;
        const char *l = read_all(b, sz);
        if (strcmp(l, whole)) {
            CHECK(false);
            fprintf(stderr, "  %s in pieces of %zu:\n%s\n  whole:\n%s\n", name, sz, l, whole);
            break;
        }
    }
    for (size_t sz = 4096; sz <= 4096 * 3; sz += 4096)
        CHECK(!strcmp(read_all(b, sz), whole));
    free(b.data);
    return whole;
}

#define CHECK_HAS(log, text) do {                                                           \
        CHECK(strstr((log), (text)) != NULL);                                               \
        if (!strstr((log), (text)))                                                         \
            fprintf(stderr, "  log:\n%s  wanted in it: \"%s\"\n", (log), (text));           \
    } while (0)

static char lines_hash[32], random_hash[32], deep_hash[32];

static void hashes(void) {
    blob_t text = load("text.txt"), rnd = load("random.bin");
    snprintf(lines_hash, sizeof(lines_hash), "#%016llx",
             (unsigned long long)fnv(FNV0, text.data, 10000));
    snprintf(random_hash, sizeof(random_hash), "#%016llx",
             (unsigned long long)fnv(FNV0, rnd.data, 3000));
    snprintf(deep_hash, sizeof(deep_hash), "#%016llx",
             (unsigned long long)fnv(FNV0, (const uint8_t *)"deep\n", 5));
    free(text.data);
    free(rnd.data);
}

static const char *deep_dir(void) {
    static char s[128];
    strcpy(s, "dir/d");
    memset(s + 5, 'x', 88);
    s[93] = '\0';
    return s;
}

// The tree of make-archive-fixtures.sh, as each tar wrote it.
static void check_tree(const char *log, bool with_long) {
    char want[512];
    CHECK_HAS(log, "d 0 1772600767 dir/ #");
    snprintf(want, sizeof(want), "- 10000 %d dir/lines.txt %s\n", MTIME, lines_hash);
    CHECK_HAS(log, want);
    snprintf(want, sizeof(want), "- 3000 %d dir/random.bin %s\n", MTIME, random_hash);
    CHECK_HAS(log, want);
    CHECK_HAS(log, "- 0 1772600767 dir/sub/empty #cbf29ce484222325\n");
    snprintf(want, sizeof(want), "- 5 %d %s/f", MTIME, deep_dir());
    CHECK_HAS(log, want);
    CHECK_HAS(log, deep_hash);
    if (with_long) {
        char name[200] = "long/l";
        memset(name + 6, 'z', 150);
        strcpy(name + 156, ".txt");
        snprintf(want, sizeof(want), "- 5 %d %s #", MTIME, name);
        CHECK_HAS(log, want);
    }
    CHECK(strstr(log, "error") == NULL);
    size_t len = strlen(log);
    CHECK(len >= 4 && !strcmp(log + len - 4, "end\n"));
}

// --- the formats --------------------------------------------------------------------------------

TEST(ustar_and_its_prefix) {
    char *log = read_every_way("tar/ustar.tar");
    check_tree(log, false);
    free(log);
    log = read_every_way("tar/bsd-ustar.tar");
    check_tree(log, false);
    free(log);
}

TEST(gnu_long_names) {
    char *log = read_every_way("tar/gnu.tar");
    check_tree(log, true);
    free(log);
}

TEST(pax_long_names) {
    char *log = read_every_way("tar/pax.tar");
    check_tree(log, true);
    free(log);
    log = read_every_way("tar/bsd-pax.tar");
    check_tree(log, true);
    free(log);
}

TEST(pax_global_header_is_skipped) {
    char *log = read_every_way("tar/global.tar");
    char want[256] = "- 7 1772600767 g/";
    memset(want + 17, 'n', 150);
    strcpy(want + 167, ".txt #");
    CHECK_HAS(log, want);
    CHECK(strstr(log, "ignored") == NULL);
    CHECK_HAS(log, "end\n");
    free(log);
}

TEST(v7_directories_by_their_slash) {
    char *log = read_every_way("tar/v7.tar");
    char want[256];
    snprintf(want, sizeof(want), "- 10000 %d dir/lines.txt %s\n", MTIME, lines_hash);
    CHECK_HAS(log, want);
    CHECK_HAS(log, "d 0 1772600767 dir/sub/ #");
    CHECK_HAS(log, "- 0 1772600767 dir/sub/empty #");
    CHECK_HAS(log, "end\n");
    free(log);
}

TEST(base256_sizes_and_dates_signed_checksums) {
    char *log = read_every_way("tar/b256.tar");
    CHECK_HAS(log, "- 14 1772600767 b256.txt #");
    CHECK_HAS(log, "end\n");
    free(log);
    log = read_every_way("tar/old.tar");
    CHECK_HAS(log, "- 4 -2177452800 old.txt #");
    free(log);
    log = read_every_way("tar/signed.tar");
    CHECK_HAS(log, "- 7 1772600767 caf\xc3\xa9.txt #");
    CHECK_HAS(log, "end\n");
    free(log);
}

TEST(links_devices_fifo) {
    char *log = read_every_way("tar/links.tar");
    CHECK_HAS(log, "p 0 1772600767 dir/fifo #");
    CHECK_HAS(log, "h 0 1772600767 dir/lines.txt -> dir/hard #");
    CHECK_HAS(log, "l 0 1772600767 dir/sym -> lines.txt #");
    free(log);
    log = read_every_way("tar/evil.tar");
    CHECK_HAS(log, "c 0 1772600767 cdev #");
    CHECK_HAS(log, "b 0 1772600767 bdev #");
    CHECK_HAS(log, "l 0 1772600767 sl -> /etc/passwd #");
    CHECK_HAS(log, "- 5 1772600767 ../evil.txt #");      // names as they are: rules come after
    CHECK_HAS(log, "/../../long-up.txt #");
    free(log);
    log = read_every_way("tar/pax-evil.tar");
    CHECK_HAS(log, "- 3 1772600767 ../pax-up.txt #");
    CHECK_HAS(log, "l 0 1772600767 sl -> ../qqqq");
    free(log);
}

TEST(too_long_is_refused_not_cut) {
    char *log = read_every_way("tar/toolong.tar");
    CHECK_HAS(log, "ccc.txt #");                        // 260 bytes: fits, FAT rules refuse it
    CHECK_HAS(log, " !name too long #");                // 803 bytes: the parser refuses it
    CHECK_HAS(log, "- 3 1772600767 ok.txt #");
    free(log);
    log = read_every_way("tar/paxbig.tar");
    CHECK_HAS(log, "- 3 1772600767 big-pax.txt !pax header too large #");
    CHECK_HAS(log, "- 3 1772600767 ok.txt #");
    CHECK_HAS(log, "end\n");
    free(log);
}

TEST(damage) {
    char *log = read_every_way("tar/badsum.tar");
    CHECK_HAS(log, "error: bad header at 1024\n");
    free(log);
    log = read_every_way("tar/cut.tar");
    CHECK_HAS(log, "error: archive ends early at 6000\n");
    free(log);
    log = read_every_way("tar/noend.tar");
    CHECK_HAS(log, "dir/sub/empty #");
    CHECK_HAS(log, "error: archive ends early at ");
    free(log);
    static uint8_t none[1];
    blob_t b = {none, 0};
    CHECK(!strcmp(read_all(b, 1), "error: archive ends early at 0\n"));
}

// --- crafted headers ----------------------------------------------------------------------------

static uint8_t arc[8192];

static void sum(uint8_t *h) {
    memset(h + 148, ' ', 8);
    unsigned s = 0;
    for (int i = 0; i < 512; i++)
        s += h[i];
    snprintf((char *)h + 148, 8, "%06o", s);
}

// A header at arc + off: name, type, size field text (or NULL for octal of `size`).
static void hdr(size_t off, const char *name, char type, uint64_t size) {
    uint8_t *h = arc + off;
    memset(h, 0, 512);
    memcpy(h, name, strlen(name));
    snprintf((char *)h + 124, 12, "%011llo", (unsigned long long)size);
    snprintf((char *)h + 136, 12, "%011o", 1u);
    h[156] = (uint8_t)type;
    memcpy(h + 257, "ustar\0" "00", 8);
    sum(h);
}

TEST(numbers_and_fields) {
    memset(arc, 0, sizeof(arc));
    hdr(0, "a", '0', 3);
    memcpy(arc + 512, "abc", 3);
    blob_t b = {arc, 512 * 4};
    CHECK_HAS(read_all(b, 512 * 4), "- 3 1 a #");
    // Octal with leading spaces and a space at the end; base-256 too large; junk.
    memcpy(arc + 124, "   00000003 ", 12);
    sum(arc);
    CHECK_HAS(read_all(b, 77), "- 3 1 a #");
    memcpy(arc + 124, "\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xfd", 12);   // -3
    sum(arc);
    CHECK_HAS(read_all(b, 77), "error: bad header at 0\n");
    memcpy(arc + 124, "\x80\x7f\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff", 12);   // over 2^63
    sum(arc);
    CHECK_HAS(read_all(b, 77), "error: bad header at 0\n");
    memcpy(arc + 124, "0000000x003\0", 12);
    sum(arc);
    CHECK_HAS(read_all(b, 77), "error: bad header at 0\n");
    // A bad date is taken as 0, not refused.
    hdr(0, "a", '0', 3);
    memcpy(arc + 136, "zzzz", 4);
    sum(arc);
    CHECK_HAS(read_all(b, 77), "- 3 0 a #");
    // A lone zero block between entries is passed over; one at the end is the end.
    memset(arc, 0, sizeof(arc));
    hdr(0, "a", '0', 0);
    hdr(1024, "b", '0', 0);
    b.len = 512 * 5;
    CHECK(!strcmp(read_all(b, 100), "- 0 1 a #cbf29ce484222325\n- 0 1 b #cbf29ce484222325\nend\n"));
    b.len = 512 * 4;
    CHECK(!strcmp(read_all(b, 100), "- 0 1 a #cbf29ce484222325\n- 0 1 b #cbf29ce484222325\nend\n"));
    b.len = 512 * 3;
    CHECK_HAS(read_all(b, 100), "error: archive ends early at 1536\n");
    // A directory's size is no data; an unknown type's is skipped as data.
    memset(arc, 0, sizeof(arc));
    hdr(0, "d/", '5', 512);
    hdr(512, "s", 'S', 512);
    hdr(1536, "z", '0', 0);
    b.len = 512 * 6;
    CHECK(!strcmp(read_all(b, 100), "d 0 1 d/ #cbf29ce484222325\n? 512 1 s #7da144b97d054b25\n"
                                    "- 0 1 z #cbf29ce484222325\nend\n"));
    // GNU magic: no prefix joined.
    memset(arc, 0, sizeof(arc));
    hdr(0, "n", '0', 0);
    memcpy(arc + 257, "ustar  \0", 8);
    memcpy(arc + 345, "pre", 3);
    sum(arc);
    b.len = 512 * 3;
    CHECK_HAS(read_all(b, 100), "- 0 1 n #");
    memcpy(arc + 257, "ustar\0" "00", 8);
    sum(arc);
    CHECK_HAS(read_all(b, 100), "- 0 1 pre/n #");
    // Data after the end is ignored.
    memset(arc + 512 * 3, 'x', 512);
    b.len = 512 * 4;
    CHECK_HAS(read_all(b, 100), "end\n");
}

TEST(pax_records) {
    static const struct {
        const char *rec;
        size_t len;             // the record's bytes (it may hold a NUL)
        uint64_t size;          // the header's size field
        const char *want;
    } cases[] = {
        {"12 path=p/q\n", 12, 2, "- 2 1 p/q #"},
        {"10 size=2\n", 10, 0, "- 2 1 a #"},              // the record's size wins
        {"8 path=\n", 8, 2, "- 2 1 a #"},                 // empty: the header's name stands
        {"14 linkpath=l\n", 14, 2, "- 2 1 a #"},          // not a link: no target shown
        {"16 path=x\nyz=1\n", 15, 2, "error: bad pax header at 0\n"},  // past the end
        {"11 path=x\n", 10, 2, "error: bad pax header at 0\n"},
        {"9 path=x\n", 10, 2, "error: bad pax header at 0\n"},   // no '\n' where it ends
        {"8 pathx\n", 8, 2, "error: bad pax header at 0\n"},     // no '='
        {"x path=x\n", 9, 2, "error: bad pax header at 0\n"},
        {"10 size=x\n", 10, 2, "error: bad pax header at 0\n"},
        {"12 path=a\0b\n", 12, 2, "- 2 1  !not a valid FAT name #"},
        {"12 mtime=99\n12 path=p/q\n", 24, 2, "- 2 1 p/q #"},   // other keys ignored
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        memset(arc, 0, sizeof(arc));
        size_t rlen = cases[i].len;
        hdr(0, "PaxHeader", 'x', rlen);
        memcpy(arc + 512, cases[i].rec, rlen);
        hdr(1024, "a", '0', cases[i].size);
        memcpy(arc + 1536, "hi", 2);
        blob_t b = {arc, 512 * 6};
        const char *l = read_all(b, 5);
        if (!strstr(l, cases[i].want)) {
            CHECK(false);
            fprintf(stderr, "  case %zu: %s  wanted: %s\n", i, l, cases[i].want);
        }
    }
}

TEST(gnu_long_name_record) {
    memset(arc, 0, sizeof(arc));
    hdr(0, "././@LongLink", 'L', 6);
    memcpy(arc + 512, "x/../y", 6);                     // no NUL: the size ends it
    hdr(1024, "short", '0', 0);
    hdr(1536, "next", '0', 0);
    blob_t b = {arc, 512 * 5};
    CHECK(!strcmp(read_all(b, 3), "- 0 1 x/../y #cbf29ce484222325\n- 0 1 next #cbf29ce484222325\n"
                                  "end\n"));
    hdr(0, "././@LongLink", 'L', 2000);                 // too long: refused, data skipped
    memset(arc + 512, 'n', 2000);
    hdr(2560, "short", '0', 0);
    b.len = 512 * 8;
    CHECK(!strcmp(read_all(b, 300), "- 0 1  !name too long #cbf29ce484222325\nend\n"));
}

// --- names --------------------------------------------------------------------------------------

TEST(name_rules) {
    static const struct {
        const char *in, *out, *why;
        bool stripped;
    } cases[] = {
        {"a/b.txt", "a/b.txt", NULL, false},
        {"/a/b", "a/b", NULL, true},
        {"///a", "a", NULL, true},
        {"./a/./b//c/", "a/b/c", NULL, false},
        {"./", "", NULL, false},
        {"/", "", NULL, true},
        {"..", NULL, "unsafe path", false},
        {"a/../b", NULL, "unsafe path", false},
        {"/../etc", NULL, "unsafe path", true},
        {"a/..", NULL, "unsafe path", false},
        {"...", NULL, "not a valid FAT name", false},       // ends in a dot
        {"a/b:c", NULL, "not a valid FAT name", false},
        {"a\\..\\b", NULL, "not a valid FAT name", false},
        {"trail ", NULL, "not a valid FAT name", false},
        {"x\x01", NULL, "not a valid FAT name", false},
        {"bad\xff", NULL, "not a valid FAT name", false},
        {"q?", NULL, "not a valid FAT name", false},
        {"caf\xc3\xa9/ok", "caf\xc3\xa9/ok", NULL, false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char out[256] = "unchanged";
        bool stripped = false;
        const char *why = tar_name_clean(cases[i].in, out, sizeof(out), &stripped);
        bool ok = (why == NULL) == (cases[i].why == NULL) && (!why || !strcmp(why, cases[i].why)) &&
                  (why || !strcmp(out, cases[i].out)) && stripped == cases[i].stripped;
        CHECK(ok);
        if (!ok)
            fprintf(stderr, "  \"%s\": %s \"%s\" %d\n", cases[i].in, why ? why : "ok", out, stripped);
    }
    char name[300], out[300];
    bool s = false;
    memset(name, 'n', 255);
    name[255] = '\0';
    CHECK(tar_name_clean(name, out, 256, &s) == NULL);
    strcpy(name + 255, "m");
    CHECK(!strcmp(tar_name_clean(name, out, 256, &s), "name too long"));
    memset(name, 'n', 200);                             // two components over the path limit
    strcpy(name + 200, "/");
    memset(name + 201, 'm', 60);
    name[261] = '\0';
    CHECK(!strcmp(tar_name_clean(name, out, 256, &s), "name too long"));
    CHECK(tar_name_clean(name, out, sizeof(out), &s) == NULL);
}

TEST(kinds_skipped) {
    char buf[24];
    CHECK(tar_kind_skipped(TAR_FILE, '0', buf, sizeof(buf)) == NULL);
    CHECK(tar_kind_skipped(TAR_DIR, '5', buf, sizeof(buf)) == NULL);
    CHECK(!strcmp(tar_kind_skipped(TAR_SYMLINK, '2', buf, sizeof(buf)),
                  "link skipped (FAT has no links)"));
    CHECK(!strcmp(tar_kind_skipped(TAR_FIFO, '6', buf, sizeof(buf)), "FIFO skipped"));
    CHECK(!strcmp(tar_kind_skipped(TAR_OTHER, 'S', buf, sizeof(buf)), "type 'S' skipped"));
    CHECK(!strcmp(tar_kind_skipped(TAR_OTHER, '\x1b', buf, sizeof(buf)), "type 0x1b skipped"));
}

TEST_MAIN(
    hashes();
    RUN(ustar_and_its_prefix);
    RUN(gnu_long_names);
    RUN(pax_long_names);
    RUN(pax_global_header_is_skipped);
    RUN(v7_directories_by_their_slash);
    RUN(base256_sizes_and_dates_signed_checksums);
    RUN(links_devices_fifo);
    RUN(too_long_is_refused_not_cut);
    RUN(damage);
    RUN(numbers_and_fields);
    RUN(pax_records);
    RUN(gnu_long_name_record);
    RUN(name_rules);
    RUN(kinds_skipped);
)
