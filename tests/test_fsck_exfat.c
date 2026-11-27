// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/files/fsck_exfat.c, the exFAT parts of the card checker. The images are
// made by FatFs itself (ff.c and ffunicode.c, built for the host over a RAM disk) with `sd
// format`'s parameters (n_fat = 1, align = 8192, with and without FM_SFD), so the pieces are
// held to what the board writes. fsck.c strings the pieces together; its tests
// (tests/test_fsck.c) run every damage case of fuzz/exfat_damage.h end to end.

#include <stdlib.h>

#include "diskio.h"
#include "ff.h"
#include "files/fsck_exfat.h"
#include "fuzz/exfat_damage.h"
#include "test.h"

// --- a RAM disk: sectors allocated when first written ---------------------------------------

typedef struct {
    uint64_t sectors;
    uint8_t **s;
} disk_t;

static const uint8_t zero_sector[512];

static disk_t *disk_new(uint64_t sectors) {
    disk_t *d = calloc(1, sizeof(*d));
    d->sectors = sectors;
    d->s = calloc((size_t)sectors, sizeof(uint8_t *));
    return d;
}

static void disk_free(disk_t *d) {
    for (uint64_t i = 0; i < d->sectors; i++)
        free(d->s[i]);
    free(d->s);
    free(d);
}

static uint8_t *disk_rw(void *ctx, uint64_t lba) {
    disk_t *d = ctx;
    if (!d->s[lba])
        d->s[lba] = calloc(1, 512);
    return d->s[lba];
}

static const uint8_t *disk_ro(const disk_t *d, uint64_t lba) {
    return lba < d->sectors && d->s[lba] ? d->s[lba] : zero_sector;
}

// --- FatFs's side: diskio, time, memory -----------------------------------------------------

static disk_t *cur;

DSTATUS disk_initialize(BYTE pdrv) {
    return pdrv == 0 && cur ? 0 : STA_NOINIT;
}

DSTATUS disk_status(BYTE pdrv) {
    return pdrv == 0 && cur ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv || !cur || sector + count > cur->sectors)
        return RES_PARERR;
    for (UINT i = 0; i < count; i++)
        memcpy(buff + 512 * i, disk_ro(cur, sector + i), 512);
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv || !cur || sector + count > cur->sectors)
        return RES_PARERR;
    for (UINT i = 0; i < count; i++) {
        if (!cur->s[sector + i] && !memcmp(buff + 512 * i, zero_sector, 512))
            continue;
        memcpy(disk_rw(cur, sector + i), buff + 512 * i, 512);
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv || !cur)
        return RES_PARERR;
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = cur->sectors;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    }
    return RES_PARERR;
}

DWORD get_fattime(void) {
    return (DWORD)(46u << 25 | 11u << 21 | 27u << 16);     // 2026-11-27 00:00
}

void *ff_memalloc(UINT msize) {
    return malloc(msize);
}

void ff_memfree(void *mblock) {
    free(mblock);
}

// --- making images --------------------------------------------------------------------------

static FATFS fs;
static uint8_t work[32768];

static void put_file(const char *path, uint32_t size) {
    FIL fp;
    CHECK_EQ(f_open(&fp, path, FA_WRITE | FA_OPEN_APPEND), FR_OK);
    uint8_t buf[4096];
    for (uint32_t done = 0; done < size;) {
        UINT n = size - done < sizeof(buf) ? size - done : sizeof(buf), wrote;
        for (UINT i = 0; i < n; i++)
            buf[i] = (uint8_t)(done + i);
        CHECK_EQ(f_write(&fp, buf, n, &wrote), FR_OK);
        done += n;
    }
    CHECK_EQ(f_close(&fp), FR_OK);
}

static const char *const long_name = "A long file name, longer than thirty characters.txt";
static const char *const greek = "Ünïcödé ñame — ελληνικά.txt";
static const char *const emoji = "emoji \xF0\x9F\x98\x80 name.txt";

// Format `sectors` as `sd format` does (`sfd`: no partition table) and fill it.
static disk_t *make_image(uint64_t sectors, bool sfd) {
    disk_t *d = disk_new(sectors);
    cur = d;
    MKFS_PARM opt = {.fmt = (BYTE)(FM_EXFAT | (sfd ? FM_SFD : 0)), .n_fat = 1, .align = 8192};
    CHECK_EQ(f_mkfs("0:", &opt, work, sizeof(work)), FR_OK);
    CHECK_EQ(f_mount(&fs, "0:", 1), FR_OK);
    CHECK_EQ(f_mkdir("dir"), FR_OK);
    CHECK_EQ(f_mkdir("dir/sub"), FR_OK);
    CHECK_EQ(f_mkdir("dir/sub/deeper"), FR_OK);
    CHECK_EQ(f_mkdir("dir/many"), FR_OK);
    put_file("README.txt", 100);
    put_file(long_name, 5000);
    put_file(greek, 3000);
    put_file("日本語のファイル.bin", 700);
    put_file(emoji, 10);
    put_file("dir/empty.txt", 0);
    put_file("dir/sub/contig.bin", 200000);         // one write: NoFatChain
    put_file("dir/frag.bin", 9000);                 // then something after it,
    put_file("dir/other.bin", 9000);
    put_file("dir/frag.bin", 9000);                 // then more: a FAT chain
    put_file("dir/sub/deeper/deep.txt", 1);
    for (int i = 0; i < 60; i++) {                  // enough to need a second cluster
        char p[80];
        snprintf(p, sizeof(p), "dir/many/file number %02d with a longer name.txt", i);
        put_file(p, (uint32_t)(i * 100));
    }
    CHECK_EQ(f_unmount("0:"), FR_OK);
    cur = NULL;
    return d;
}

static disk_t *img_mbr, *img_sfd, *img_big;

static uint64_t find_volume(const disk_t *d) {
    const uint8_t *s0 = disk_ro(d, 0);
    if (fsck_exfat_is_vbr(s0))
        return 0;
    return exd_le32(s0 + 446 + 8);
}

static bool open_x(exd_t *x, disk_t *d) {
    return exd_open(x, disk_rw, d, find_volume(d), d->sectors);
}

static void find_ok(const exd_t *x, const char *path, exd_loc_t *loc) {
    bool found = exd_find(x, path, loc);
    if (!found)
        fprintf(stderr, "  %s: %s not found\n", test_current, path);
    CHECK(found);
}

// --- tests: images FatFs wrote ------------------------------------------------------------

TEST(geometry) {
    exd_t x;
    CHECK(open_x(&x, img_mbr));
    CHECK(x.g.volume_lba != 0);                     // a partition table before the volume
    CHECK_EQ(x.g.cluster_bytes, 4096);
    CHECK_EQ(x.g.heap_offset % 8192 == 0 || (x.g.volume_lba + x.g.heap_offset) % 8192 == 0, 1);
    CHECK(open_x(&x, img_sfd));
    CHECK_EQ(x.g.volume_lba, 0);
    CHECK(open_x(&x, img_big));
    CHECK_EQ(x.g.cluster_bytes, 32768);             // 64 sectors from 512 K sectors on
    CHECK_EQ(x.g.volume_flags, 0);
}

TEST(sets_as_written) {
    exd_t x;
    CHECK(open_x(&x, img_mbr));
    exd_loc_t l;
    find_ok(&x, "dir/sub/contig.bin", &l);
    CHECK(l.set.no_fat_chain);
    CHECK_EQ(l.set.length, 200000);
    CHECK_EQ(l.set.clusters, 49);
    CHECK_EQ(l.set.run_clusters, 49);
    CHECK(!l.set.dir);
    find_ok(&x, "dir/frag.bin", &l);
    CHECK(!l.set.no_fat_chain);
    CHECK_EQ(l.set.length, 18000);
    CHECK_EQ(l.set.clusters, 5);
    find_ok(&x, "dir/empty.txt", &l);
    CHECK_EQ(l.set.length, 0);
    CHECK_EQ(l.set.first_cluster, 0);
    CHECK_EQ(l.set.clusters, 0);
    find_ok(&x, "dir/sub", &l);
    CHECK(l.set.dir);
    CHECK_EQ(l.set.problems, 0);
    find_ok(&x, long_name, &l);
    CHECK_EQ(l.set.name_len, strlen(long_name));
    CHECK_EQ(l.set.name_entries, 4);
    CHECK_EQ(l.set.secondaries, 5);
    find_ok(&x, emoji, &l);
    CHECK_EQ(l.set.name_len, 17);                   // the emoji is a surrogate pair
    find_ok(&x, greek, &l);
    find_ok(&x, "dir/many/file number 59 with a longer name.txt", &l);
    CHECK(l.set.index >= 128);                      // in the directory's second cluster
    // The hash FatFs stored is the one computed with its ff_wtoupper().
    uint16_t name[64];
    size_t n = exd_utf16(greek, greek + strlen(greek), name, 64);
    find_ok(&x, greek, &l);
    CHECK_EQ(fsck_exfat_name_hash(name, n, ff_wtoupper), l.set.hash_stored);
}

// --- tests: the pieces on their own ---------------------------------------------------------

TEST(vbr_refusals) {
    exd_t x;
    CHECK(open_x(&x, img_sfd));
    const uint8_t *orig = disk_ro(img_sfd, 0);
    uint64_t dev = img_sfd->sectors;
    fsck_exfat_geom_t g;
    uint8_t s[512];
    struct {
        size_t at;
        int bytes;
        uint64_t val;
        const char *why;
    } cases[] = {
        {3, 1, 'X', "not an exFAT boot sector"},
        {510, 1, 0, "not an exFAT boot sector"},
        {40, 1, 1, "bytes 11-63 of the boot sector not zero"},
        {104, 2, 0x200, "exFAT version not 1.0"},
        {108, 1, 12, "bytes per sector not 512"},
        {110, 1, 2, "number of FATs not 1"},
        {109, 1, 16, "cluster size over 32768 sectors"},
        {92, 4, 0x7FFFFFFE, "too many clusters for exFAT"},
        {72, 8, 1000, "volume smaller than its cluster heap"},
        {92, 4, 0, "cluster count 0"},
        {80, 4, 23, "FAT inside the boot regions"},
        {84, 4, 1, "FAT too small for the cluster count"},
        {80, 4, 9000, "FAT overlaps the cluster heap"},
        {96, 4, 1, "root directory cluster out of range"},
        {96, 4, x.g.clusters + 2, "root directory cluster out of range"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        memcpy(s, orig, 512);
        for (int k = 0; k < cases[i].bytes; k++)
            s[cases[i].at + (size_t)k] = (uint8_t)(cases[i].val >> (8 * k));
        const char *why = fsck_exfat_vbr(s, 0, dev, &g);
        if (!why || strcmp(why, cases[i].why))
            fprintf(stderr, "  case %zu: got \"%s\"\n", i, why ? why : "(accepted)");
        CHECK(why && !strcmp(why, cases[i].why));
    }
    CHECK(fsck_exfat_vbr(orig, 0, dev, &g) == NULL);
    CHECK_EQ(strcmp(fsck_exfat_vbr(orig, 0, dev - 1, &g), "volume larger than the device"), 0);
    CHECK_EQ(strcmp(fsck_exfat_vbr(orig, 1, dev, &g), "volume larger than the device"), 0);
}

TEST(boot_region_fed) {
    // Sectors fed after the 24th are ignored; the backup is compared with the main region.
    fsck_exfat_boot_t b;
    fsck_exfat_boot_init(&b);
    for (uint32_t i = 0; i < 30; i++)
        fsck_exfat_boot_sector(&b, disk_ro(img_sfd, i));
    CHECK_EQ(fsck_exfat_boot_done(&b), 0);
    // VolumeFlags and PercentInUse may change in the main region only.
    uint8_t s0[512];
    memcpy(s0, disk_ro(img_sfd, 0), 512);
    s0[106] = 0x06;
    s0[112] = 50;
    fsck_exfat_boot_init(&b);
    fsck_exfat_boot_sector(&b, s0);
    for (uint32_t i = 1; i < 24; i++)
        fsck_exfat_boot_sector(&b, disk_ro(img_sfd, i));
    CHECK_EQ(fsck_exfat_boot_done(&b), 0);
}

static uint32_t ascii_upcase(uint32_t c) {
    return c >= 'a' && c <= 'z' ? c - 32 : c;
}

// The up-case table as an uncompressed list, from a mapping.
static uint8_t *full_table(uint32_t (*map)(uint32_t), uint32_t *sum) {
    uint8_t *t = malloc(131072);
    *sum = 0;
    for (uint32_t c = 0; c < 65536; c++) {
        uint32_t m = map(c);
        t[2 * c] = (uint8_t)m;
        t[2 * c + 1] = (uint8_t)(m >> 8);
    }
    for (uint32_t i = 0; i < 131072; i++)
        *sum = ((*sum & 1) ? 0x80000000u : 0) + (*sum >> 1) + t[i];
    return t;
}

TEST(upcase_table) {
    uint32_t sum;
    uint8_t *t = full_table(ff_wtoupper, &sum);
    fsck_exfat_upcase_t u;
    // Uncompressed except for its last entry, 0xFFFF, read as a count with nothing after it.
    fsck_exfat_upcase_init(&u, 131072, ff_wtoupper);
    fsck_exfat_upcase_feed(&u, t, 131072);
    CHECK_EQ(fsck_exfat_upcase_done(&u, sum), 0);
    // A byte at a time, split units and all.
    fsck_exfat_upcase_init(&u, 131072, ff_wtoupper);
    for (uint32_t i = 0; i < 131072; i++)
        fsck_exfat_upcase_feed(&u, t + i, 1);
    CHECK_EQ(fsck_exfat_upcase_done(&u, sum), 0);
    CHECK_EQ(fsck_exfat_upcase_done(&u, sum + 1), FSCK_EXFAT_U_CHECKSUM);
    free(t);
    // A table that maps only ASCII: the rest of FatFs's mappings differ.
    t = full_table(ascii_upcase, &sum);
    fsck_exfat_upcase_init(&u, 131072, ff_wtoupper);
    fsck_exfat_upcase_feed(&u, t, 131072);
    CHECK_EQ(fsck_exfat_upcase_done(&u, sum), FSCK_EXFAT_U_DIFFERS);
    CHECK_EQ(u.first_differ, 0xE0);
    char text[FSCK_EXFAT_TEXT_MAX];
    fsck_exfat_upcase_text(&u, FSCK_EXFAT_U_DIFFERS, text, sizeof(text));
    CHECK(strstr(text, "(the first U+00E0)") != NULL);
    // A short table: what it leaves out maps to itself, which FatFs's mapping doesn't.
    fsck_exfat_upcase_init(&u, 256, ff_wtoupper);
    fsck_exfat_upcase_feed(&u, t, 131072);
    CHECK_EQ(fsck_exfat_upcase_done(&u, 0) & FSCK_EXFAT_U_DIFFERS, FSCK_EXFAT_U_DIFFERS);
    CHECK_EQ(u.fed, 256);
    free(t);
}

// Entries built by hand, for the parser's corners.
static void entry(uint8_t *e, uint8_t type) {
    memset(e, 0, 32);
    e[0] = type;
}

static fsck_exfat_geom_t small_geom = {.clusters = 1000, .cluster_bytes = 4096,
                                       .cluster_sectors = 8, .root_cluster = 4};

// A file entry set for an ASCII name, with its checksum and hash: n entries into es.
static int make_set(uint8_t es[][32], const char *name, uint32_t first, uint64_t len,
                    bool contiguous) {
    size_t nl = strlen(name);
    int names = (int)((nl + 14) / 15);
    entry(es[0], 0x85);
    es[0][1] = (uint8_t)(1 + names);
    entry(es[1], 0xC0);
    es[1][1] = (uint8_t)(contiguous ? 3 : 1);
    es[1][3] = (uint8_t)nl;
    uint16_t u16[255];
    for (size_t i = 0; i < nl; i++)
        u16[i] = (uint8_t)name[i];
    exd_put16(es[1] + 4, fsck_exfat_name_hash(u16, nl, ascii_upcase));
    exd_put64(es[1] + 8, len);
    exd_put32(es[1] + 20, first);
    exd_put64(es[1] + 24, len);
    for (int k = 0; k < names; k++) {
        entry(es[2 + k], 0xC1);
        for (size_t i = 0; i < 15 && 15 * (size_t)k + i < nl; i++)
            exd_put16(es[2 + k] + 2 + 2 * i, u16[15 * (size_t)k + i]);
    }
    uint16_t sum = 0;
    for (int k = 0; k < 2 + names; k++)
        for (int i = 0; i < 32; i++)
            if (k || (i != 2 && i != 3))
                sum = (uint16_t)(((sum & 1) ? 0x8000u : 0) + (sum >> 1) + es[k][i]);
    exd_put16(es[0] + 2, sum);
    return 2 + names;
}

// Feed `n` entries; the results ORed, and the problems of every set and directory call ORed.
typedef struct {
    uint32_t results, set_problems, dir_problems;
    int sets;
} fed_t;

static fed_t feed(fsck_exfat_dir_t *pd, uint8_t es[][32], int n, bool end) {
    fed_t f = {0};
    for (int i = 0; i < n; i++) {
        uint32_t r;
        do {
            r = fsck_exfat_entry(pd, es[i]);
            f.results |= r;
            if (r & FSCK_EXFAT_SET) {
                f.sets++;
                f.set_problems |= pd->set.problems;
            }
            f.dir_problems |= pd->problems;
        } while (r & FSCK_EXFAT_AGAIN);
    }
    if (end) {
        uint32_t r = fsck_exfat_dir_end(pd);
        f.results |= r;
        if (r & FSCK_EXFAT_SET) {
            f.sets++;
            f.set_problems |= pd->set.problems;
        }
        f.dir_problems |= pd->problems;
    }
    return f;
}

TEST(parser_corners) {
    fsck_exfat_dir_t pd;
    uint8_t es[40][32];
    // A clean set, then the end entry.
    int n = make_set(es, "hello.txt", 10, 5000, true);
    entry(es[n], 0x00);
    fsck_exfat_dir_begin(&pd, &small_geom, ascii_upcase, false);
    fed_t f = feed(&pd, es, n + 1, true);
    CHECK_EQ(f.sets, 1);
    CHECK_EQ(f.set_problems, 0);
    CHECK_EQ(f.dir_problems, 0);
    CHECK(f.results & FSCK_EXFAT_END);
    CHECK_EQ(pd.set.run_clusters, 2);
    // After the end entry nothing more is read.
    CHECK_EQ(fsck_exfat_entry(&pd, es[0]), FSCK_EXFAT_END);

    // A benign primary's secondaries are skipped; benign secondaries alone too; deleted ones.
    entry(es[0], 0xA0);
    es[0][1] = 2;
    entry(es[1], 0xC0);                             // would be stray if not the 0xA0's
    entry(es[2], 0xC1);
    entry(es[3], 0xE0);
    entry(es[4], 0x05);
    n = 5 + make_set(es + 5, "x", 0, 0, false);
    fsck_exfat_dir_begin(&pd, &small_geom, ascii_upcase, false);
    f = feed(&pd, es, n, true);
    CHECK_EQ(f.sets, 1);
    CHECK_EQ(f.set_problems, 0);
    CHECK_EQ(f.dir_problems, 0);
    CHECK_EQ(pd.set.index, 5);

    // A stray critical secondary is reported once per run.
    entry(es[0], 0xC1);
    entry(es[1], 0xC1);
    fsck_exfat_dir_begin(&pd, &small_geom, ascii_upcase, false);
    CHECK_EQ(fsck_exfat_entry(&pd, es[0]), FSCK_EXFAT_DIR);
    CHECK_EQ(pd.problems, FSCK_EXFAT_D_STRAY);
    CHECK_EQ(fsck_exfat_entry(&pd, es[1]), 0);

    // A set cut short by the chain's end, and a deleted entry inside a set.
    n = make_set(es, "abc", 10, 1, false);
    fsck_exfat_dir_begin(&pd, &small_geom, ascii_upcase, false);
    f = feed(&pd, es, n - 1, true);
    CHECK_EQ(f.sets, 1);
    CHECK_EQ(f.set_problems, FSCK_EXFAT_S_SHORT);
    CHECK(pd.set.usable);
    es[n - 1][0] = 0x41;
    fsck_exfat_dir_begin(&pd, &small_geom, ascii_upcase, false);
    CHECK_EQ(fsck_exfat_entry(&pd, es[0]), 0);
    CHECK_EQ(fsck_exfat_entry(&pd, es[1]), 0);
    CHECK_EQ(fsck_exfat_entry(&pd, es[2]), FSCK_EXFAT_SET | FSCK_EXFAT_AGAIN);
    CHECK_EQ(fsck_exfat_entry(&pd, es[2]), 0);      // again: now just a deleted entry

    // A vendor extension (benign secondary) after the names: the spec allows it, FatFs not.
    n = make_set(es, "abc", 10, 1, false);
    es[0][1]++;
    entry(es[n++], 0xE0);
    fsck_exfat_dir_begin(&pd, &small_geom, ascii_upcase, false);
    f = feed(&pd, es, n, true);
    CHECK_EQ(f.set_problems, FSCK_EXFAT_S_NOT_NAME | FSCK_EXFAT_S_CHECKSUM);
    char text[FSCK_EXFAT_TEXT_MAX];
    fsck_exfat_set_text(&pd.set, FSCK_EXFAT_S_NOT_NAME, text, sizeof(text));
    CHECK(!strcmp(text, "entry type 0xE0 where file name entry 2 should be"));

    // No stream entry second.
    n = make_set(es, "abc", 10, 1, false);
    es[1][0] = 0xC1;
    fsck_exfat_dir_begin(&pd, &small_geom, ascii_upcase, false);
    f = feed(&pd, es, n, true);
    CHECK(f.set_problems & FSCK_EXFAT_S_NO_STREAM);
    CHECK(!pd.set.usable);

    // Names: a refused character, a broken surrogate pair, "..", and an empty name.
    const char *bad[] = {"a:b", "..", "", NULL};
    uint32_t want[] = {FSCK_EXFAT_S_NAME_CHAR, FSCK_EXFAT_S_NAME_DOT, FSCK_EXFAT_S_NAME_EMPTY};
    for (int i = 0; bad[i]; i++) {
        n = make_set(es, bad[i], 0, 0, false);
        if (!*bad[i]) {                             // no name entry at all: give it one
            es[0][1] = 2;
            entry(es[2], 0xC1);
            n = 3;
            uint16_t sum = 0;
            for (int k = 0; k < 3; k++)
                for (int b = 0; b < 32; b++)
                    if (k || (b != 2 && b != 3))
                        sum = (uint16_t)(((sum & 1) ? 0x8000u : 0) + (sum >> 1) + es[k][b]);
            exd_put16(es[0] + 2, sum);
        }
        fsck_exfat_dir_begin(&pd, &small_geom, ascii_upcase, false);
        f = feed(&pd, es, n, true);
        CHECK_EQ(f.set_problems, want[i]);
    }
    n = make_set(es, "ab", 0, 0, false);
    exd_put16(es[2] + 2, 0xD800);                   // a high surrogate with "b" after it
    fsck_exfat_dir_begin(&pd, &small_geom, ascii_upcase, false);
    f = feed(&pd, es, n, true);
    CHECK(f.set_problems & FSCK_EXFAT_S_NAME_CHAR);
    CHECK_EQ(pd.set.bad_char, 0xD800);

    // Sizes: first cluster out of range, size 0 with a cluster, too big for the volume.
    struct {
        uint32_t first;
        uint64_t len;
        uint32_t want;
    } sz[] = {
        {1, 100, FSCK_EXFAT_S_FIRST_CLUSTER},
        {1002, 100, FSCK_EXFAT_S_FIRST_CLUSTER},
        {5, 0, FSCK_EXFAT_S_FIRST_CLUSTER},
        {2, 1001ull * 4096, FSCK_EXFAT_S_TOO_BIG},
        {2, 1000ull * 4096, 0},
        {3, 1000ull * 4096, FSCK_EXFAT_S_PAST_HEAP},
        {2, UINT64_MAX, FSCK_EXFAT_S_TOO_BIG},
    };
    for (size_t i = 0; i < sizeof(sz) / sizeof(sz[0]); i++) {
        n = make_set(es, "f", sz[i].first, sz[i].len, true);
        fsck_exfat_dir_begin(&pd, &small_geom, ascii_upcase, false);
        f = feed(&pd, es, n, true);
        CHECK_EQ(f.set_problems, sz[i].want);
    }
    CHECK_EQ(pd.set.clusters, 1000);

    // The root's critical entries out of the root, and a root without them.
    entry(es[0], 0x81);
    entry(es[1], 0x82);
    fsck_exfat_dir_begin(&pd, &small_geom, ascii_upcase, false);
    CHECK_EQ(fsck_exfat_entry(&pd, es[0]), FSCK_EXFAT_DIR);
    CHECK_EQ(pd.problems, FSCK_EXFAT_D_ROOT_ONLY);
    fsck_exfat_dir_begin(&pd, &small_geom, ascii_upcase, true);
    entry(es[0], 0x00);
    f = feed(&pd, es, 1, true);
    CHECK_EQ(f.dir_problems, FSCK_EXFAT_D_NO_BITMAP | FSCK_EXFAT_D_NO_UPCASE);

    // A secondary count of 0.
    entry(es[0], 0x85);
    fsck_exfat_dir_begin(&pd, &small_geom, ascii_upcase, false);
    CHECK_EQ(fsck_exfat_entry(&pd, es[0]), FSCK_EXFAT_SET);
    CHECK(pd.set.problems & FSCK_EXFAT_S_COUNT);
}

TEST(runs_and_windows) {
    // fsck_run_add(): consecutive clusters make one run.
    fsck_run_t r = {0}, done;
    CHECK(!fsck_run_add(&r, 5, &done));
    CHECK(!fsck_run_add(&r, 6, &done));
    CHECK(fsck_run_add(&r, 9, &done));
    CHECK_EQ(done.first, 5);
    CHECK_EQ(done.count, 2);
    CHECK(fsck_run_end(&r, &done));
    CHECK_EQ(done.first, 9);
    CHECK(!fsck_run_end(&r, &done));

    // Marking: clipped to the window, cross-links counted.
    uint8_t win[4] = {0};
    uint32_t cross = 0;
    CHECK_EQ(fsck_window_mark_run(win, 100, 32, 90, 15, &cross), 0);      // 100-104
    CHECK_EQ(win[0], 0x1F);
    CHECK_EQ(fsck_window_mark_run(win, 100, 32, 103, 100, &cross), 2);    // 103-131
    CHECK_EQ(cross, 103);
    CHECK_EQ(win[3], 0xFF);
    CHECK_EQ(fsck_window_mark_run(win, 100, 32, 200, 5, &cross), 0);
    CHECK_EQ(fsck_window_mark_run(win, 100, 32, UINT32_MAX - 2, 10, &cross), 0);

    // Comparing: runs carry over from one slice to the next.
    uint8_t reached[4] = {0xF0, 0x00, 0xFF, 0x0F}, bitmap[4] = {0x0F, 0x00, 0xFF, 0xF0};
    fsck_bitcmp_t c;
    fsck_bitcmp_init(&c);
    fsck_run_t got[8];
    int kinds[8], k = 0, kind;
    fsck_bitcmp_slice(&c, reached, bitmap, 2, 16);
    while ((kind = fsck_bitcmp_next(&c, &got[k])))
        kinds[k++] = kind;
    fsck_bitcmp_slice(&c, reached + 2, bitmap + 2, 18, 16);
    while ((kind = fsck_bitcmp_next(&c, &got[k])))
        kinds[k++] = kind;
    while ((kind = fsck_bitcmp_finish(&c, &got[k])))
        kinds[k++] = kind;
    CHECK_EQ(k, 4);
    CHECK_EQ(kinds[0], FSCK_RUN_LOST);              // clusters 2-5 (bits 0-3 of byte 0)
    CHECK_EQ(got[0].first, 2);
    CHECK_EQ(got[0].count, 4);
    CHECK_EQ(kinds[1], FSCK_RUN_FREE);              // 6-9
    CHECK_EQ(got[1].first, 6);
    CHECK_EQ(kinds[2], FSCK_RUN_FREE);              // 26-29 (byte 3, bits 0-3)
    CHECK_EQ(got[2].first, 26);
    CHECK_EQ(kinds[3], FSCK_RUN_LOST);              // 30-33
    CHECK_EQ(got[3].first, 30);
    CHECK_EQ(got[3].count, 4);
    CHECK_EQ(c.lost_total, 8);
    CHECK_EQ(c.free_total, 8);
    CHECK_EQ(c.used_total, 16);

    // A run at a slice's end goes on into the next slice when that starts right after it.
    uint8_t one = 0x80, none = 0x00, all = 0x01;
    fsck_bitcmp_init(&c);
    fsck_bitcmp_slice(&c, &none, &one, 2, 8);
    CHECK_EQ(fsck_bitcmp_next(&c, &got[0]), 0);
    fsck_bitcmp_slice(&c, &none, &all, 10, 3);      // cluster 10 lost too, 11-12 fine
    CHECK_EQ(fsck_bitcmp_next(&c, &got[0]), FSCK_RUN_LOST);
    CHECK_EQ(got[0].first, 9);
    CHECK_EQ(got[0].count, 2);
    CHECK_EQ(fsck_bitcmp_next(&c, &got[0]), 0);
    CHECK_EQ(fsck_bitcmp_finish(&c, &got[0]), 0);
}

// --- the fuzz target's seeds (tests/fuzz/seeds.py) are clean --------------------------------

static size_t read_seed(const char *name, uint8_t *buf, size_t max) {
    char path[512];
    snprintf(path, sizeof(path), "%s/seed-%s", FSCK_EXFAT_SEEDS_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t n = fread(buf, 1, max, f);
    fclose(f);
    return n;
}

TEST(seeds_clean) {
    static uint8_t buf[16384];
    // boot: [0][lba][device sectors] then sectors 0-11; the backup is the same.
    size_t n = read_seed("boot", buf, sizeof(buf));
    CHECK(n > 6 + 12 * 512 - 1);
    fsck_exfat_geom_t g;
    CHECK(fsck_exfat_vbr(buf + 6, 0, exd_le32(buf + 2), &g) == NULL);
    fsck_exfat_boot_t b;
    fsck_exfat_boot_init(&b);
    for (int k = 0; k < 24; k++)
        fsck_exfat_boot_sector(&b, buf + 6 + k % 12 * 512);
    CHECK_EQ(fsck_exfat_boot_done(&b), 0);
    // root: [1][flags][clusters][shift] then entries, all clean.
    n = read_seed("root", buf, sizeof(buf));
    CHECK(n > 7);
    small_geom.clusters = exd_le32(buf + 2);
    fsck_exfat_dir_t pd;
    fsck_exfat_dir_begin(&pd, &small_geom, ascii_upcase, buf[1] & 1);
    fed_t f = feed(&pd, (uint8_t(*)[32])(buf + 7), (int)((n - 7) / 32), true);
    CHECK_EQ(f.sets, 7);                            // label, bitmap, up-case and four files
    CHECK_EQ(f.set_problems, 0);
    CHECK_EQ(f.dir_problems, 0);
    small_geom.clusters = 1000;
    // upcase: [2][length][chunk] then a table for ASCII, whose checksum the root's entry holds.
    n = read_seed("upcase", buf, sizeof(buf));
    CHECK(n > 6);
    fsck_exfat_upcase_t u;
    fsck_exfat_upcase_init(&u, exd_le32(buf + 1), ascii_upcase);
    fsck_exfat_upcase_feed(&u, buf + 6, n - 6);
    CHECK_EQ(u.fed, n - 6);
    CHECK_EQ(fsck_exfat_upcase_done(&u, u.sum), 0);
}

TEST_MAIN(
    img_mbr = make_image(131072, false);           // 64 MB, with a partition table
    img_sfd = make_image(131072, true);
    img_big = make_image(614400, true);            // 300 MB: 32 KB clusters
    RUN(geometry);
    RUN(sets_as_written);
    RUN(vbr_refusals);
    RUN(boot_region_fed);
    RUN(upcase_table);
    RUN(parser_corners);
    RUN(runs_and_windows);
    RUN(seeds_clean);
    disk_free(img_mbr);
    disk_free(img_sfd);
    disk_free(img_big);
)
