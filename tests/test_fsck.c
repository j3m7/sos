// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/files/fsck.c, the card checker. The images are made by FatFs itself
// (ff.c and ffunicode.c, built for the host over a sparse RAM disk), with the parameters `sd
// format` uses (n_fat = 1, align = 8192), so the checker is held to what the board writes.
// Each damage case breaks one thing by writing the image directly and expects the exact
// report lines. Every case is checked twice: with the board's 16 KB window, and with a tiny
// window and one read per step, which takes many passes and stops between every read; both
// must give the same lines and summary.

#include <stdarg.h>
#include <stdlib.h>

#include "diskio.h"
#include "ff.h"
#include "files/fsck.h"
#include "fuzz/exfat_damage.h"
#include "fuzz/fsck_image.h"
#include "test.h"

// --- a sparse RAM disk ----------------------------------------------------------------------

// Sectors that were written with anything but zeros, in a hash table: an image of 128 GB costs
// only what its structures and files take.
typedef struct {
    uint64_t lba;
    uint8_t *data;
} slot_t;

typedef struct {
    uint64_t sectors;
    size_t cap, used;
    slot_t *slot;
} disk_t;

static disk_t *disk_new(uint64_t sectors) {
    disk_t *d = calloc(1, sizeof(*d));
    d->sectors = sectors;
    d->cap = 1024;
    d->slot = calloc(d->cap, sizeof(slot_t));
    return d;
}

static size_t disk_hash(const disk_t *d, uint64_t lba) {
    return (size_t)((lba * 0x9E3779B97F4A7C15ull) >> 24) & (d->cap - 1);
}

static uint8_t *disk_sector(disk_t *d, uint64_t lba, bool create);

static void disk_grow(disk_t *d) {
    slot_t *old = d->slot;
    size_t n = d->cap;
    d->cap *= 2;
    d->slot = calloc(d->cap, sizeof(slot_t));
    for (size_t i = 0; i < n; i++) {
        if (!old[i].data)
            continue;
        size_t j = disk_hash(d, old[i].lba);
        while (d->slot[j].data)
            j = (j + 1) & (d->cap - 1);
        d->slot[j] = old[i];
    }
    free(old);
}

static uint8_t *disk_sector(disk_t *d, uint64_t lba, bool create) {
    size_t i = disk_hash(d, lba);
    while (d->slot[i].data) {
        if (d->slot[i].lba == lba)
            return d->slot[i].data;
        i = (i + 1) & (d->cap - 1);
    }
    if (!create)
        return NULL;
    if (2 * (d->used + 1) > d->cap) {
        disk_grow(d);
        return disk_sector(d, lba, true);
    }
    d->slot[i].lba = lba;
    d->slot[i].data = calloc(1, 512);
    d->used++;
    return d->slot[i].data;
}

static void disk_get(disk_t *d, uint64_t lba, uint8_t *buf) {
    const uint8_t *s = disk_sector(d, lba, false);
    if (s)
        memcpy(buf, s, 512);
    else
        memset(buf, 0, 512);
}

static void disk_put(disk_t *d, uint64_t lba, const uint8_t *buf) {
    uint8_t *s = disk_sector(d, lba, false);
    if (!s) {
        static const uint8_t zero[512];
        if (!memcmp(buf, zero, 512))
            return;
        s = disk_sector(d, lba, true);
    }
    memcpy(s, buf, 512);
}

static disk_t *disk_copy(const disk_t *src) {
    disk_t *d = calloc(1, sizeof(*d));
    *d = *src;
    d->slot = calloc(d->cap, sizeof(slot_t));
    for (size_t i = 0; i < d->cap; i++) {
        d->slot[i].lba = src->slot[i].lba;
        if (src->slot[i].data) {
            d->slot[i].data = malloc(512);
            memcpy(d->slot[i].data, src->slot[i].data, 512);
        }
    }
    return d;
}

static void disk_free(disk_t *d) {
    for (size_t i = 0; i < d->cap; i++)
        free(d->slot[i].data);
    free(d->slot);
    free(d);
}

// --- FatFs's side: diskio, time, memory -----------------------------------------------------

static disk_t *cur;                 // the disk FatFs sees as drive 0

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
        disk_get(cur, sector + i, buff + 512 * i);
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv || !cur || sector + count > cur->sectors)
        return RES_PARERR;
    for (UINT i = 0; i < count; i++)
        disk_put(cur, sector + i, buff + 512 * i);
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

// --- volumes: making, reading and damaging them ---------------------------------------------

// A FAT32 volume's layout, from FatFs's FATFS once mounted.
typedef struct {
    disk_t *d;
    uint64_t vol, fatbase, database;
    uint32_t csize, count, fsize, nfats, root;
} vol_t;

// Where an entry is: its short entry, its first long-name entry, its cluster and size.
typedef struct {
    uint32_t dir;               // its directory's first cluster
    uint32_t index;             // its short entry's index in the directory
    uint32_t lfn_index;         // its first long-name entry's (== index when it has none)
    uint32_t clust, size;
} loc_t;

static FATFS fs;
static uint8_t work[32768];

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void wr16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

// Format a disk of `sectors` with f_mkfs (`sd format`'s alignment), mount it, and note its
// layout. It stays mounted for filling.
static vol_t make_volume(uint64_t sectors, BYTE fmt, BYTE n_fat, DWORD au) {
    vol_t v = {.d = disk_new(sectors)};
    cur = v.d;
    MKFS_PARM opt = {.fmt = fmt, .n_fat = n_fat, .align = 8192, .au_size = au};
    CHECK_EQ(f_mkfs("0:", &opt, work, sizeof(work)), FR_OK);
    CHECK_EQ(f_mount(&fs, "0:", 1), FR_OK);
    v.vol = fs.volbase;
    v.fatbase = fs.fatbase;
    v.database = fs.database;
    v.csize = fs.csize;
    v.count = fs.n_fatent - 2;
    v.fsize = fs.fsize;
    v.nfats = fs.n_fats;
    v.root = (uint32_t)fs.dirbase;
    return v;
}

static void remount(vol_t *v) {
    cur = v->d;
    CHECK_EQ(f_mount(&fs, "0:", 1), FR_OK);
}

static void unmount(void) {
    f_mount(NULL, "0:", 0);
    cur = NULL;
}

static void put_file(const char *path, uint32_t size) {
    FIL fp;
    CHECK_EQ(f_open(&fp, path, FA_WRITE | FA_CREATE_ALWAYS), FR_OK);
    uint8_t buf[512];
    for (uint32_t done = 0; done < size;) {
        UINT n = size - done < sizeof(buf) ? size - done : sizeof(buf), wrote;
        for (UINT i = 0; i < n; i++)
            buf[i] = (uint8_t)(done + i);
        CHECK_EQ(f_write(&fp, buf, n, &wrote), FR_OK);
        done += n;
    }
    CHECK_EQ(f_close(&fp), FR_OK);
}

static void mkdir_ok(const char *path) {
    CHECK_EQ(f_mkdir(path), FR_OK);
}

static uint32_t fat_rd(const vol_t *v, uint32_t c) {
    uint8_t s[512];
    disk_get(v->d, v->fatbase + c / 128, s);
    return rd32(s + c % 128 * 4) & 0x0FFFFFFF;
}

// Set FAT entry `c` in every copy, so only the damage meant shows.
static void fat_wr(const vol_t *v, uint32_t c, uint32_t val) {
    for (uint32_t k = 0; k < v->nfats; k++) {
        uint8_t s[512];
        uint64_t lba = v->fatbase + k * v->fsize + c / 128;
        disk_get(v->d, lba, s);
        wr32(s + c % 128 * 4, val);
        disk_put(v->d, lba, s);
    }
}

static uint64_t clus_lba(const vol_t *v, uint32_t c) {
    return v->database + (uint64_t)(c - 2) * v->csize;
}

// The sector and byte of entry `index` of the directory starting at `dir`.
static void dir_pos(const vol_t *v, uint32_t dir, uint32_t index, uint64_t *lba, uint32_t *byte) {
    uint32_t off = index * 32, cb = v->csize * 512, c = dir ? dir : v->root;
    for (uint32_t i = 0; i < off / cb; i++)
        c = fat_rd(v, c);
    *lba = clus_lba(v, c) + off % cb / 512;
    *byte = off % 512;
}

// Change `n` bytes at `at` of entry `index` of directory `dir`.
static void entry_patch(const vol_t *v, uint32_t dir, uint32_t index, uint32_t ofs,
                        const void *bytes, size_t n) {
    uint64_t lba;
    uint32_t byte;
    uint8_t s[512];
    dir_pos(v, dir, index, &lba, &byte);
    disk_get(v->d, lba, s);
    memcpy(s + byte + ofs, bytes, n);
    disk_put(v->d, lba, s);
}

static void entry_set32(const vol_t *v, uint32_t dir, uint32_t index, uint32_t ofs, uint32_t val) {
    uint8_t b[4];
    wr32(b, val);
    entry_patch(v, dir, index, ofs, b, 4);
}

static void entry_set_clust(const vol_t *v, const loc_t *l, uint32_t c) {
    uint8_t b[2];
    wr16(b, c >> 16);
    entry_patch(v, l->dir, l->index, 20, b, 2);
    wr16(b, c);
    entry_patch(v, l->dir, l->index, 26, b, 2);
}

static void sector_patch(const vol_t *v, uint64_t lba, uint32_t ofs, const void *bytes, size_t n) {
    uint8_t s[512];
    disk_get(v->d, lba, s);
    memcpy(s + ofs, bytes, n);
    disk_put(v->d, lba, s);
}

// Find `name` in the directory `dir` (mounted) with f_readdir. After an item, FatFs's DIR has
// moved on to the next entry, unless it reached the end of the directory (sect 0).
static loc_t locate(const vol_t *v, const char *dir, const char *name) {
    loc_t l = {0};
    DIR dj;
    FILINFO fno;
    CHECK_EQ(f_opendir(&dj, dir), FR_OK);
    for (;;) {
        FRESULT fr = f_readdir(&dj, &fno);
        if (fr != FR_OK || !fno.fname[0]) {
            fprintf(stderr, "%s: locate: %s/%s not found\n", test_current, dir, name);
            test_checks++;
            test_failures++;
            break;
        }
        if (strcmp(fno.fname, name))
            continue;
        uint32_t off = dj.sect ? dj.dptr - 32 : dj.dptr;
        l.dir = dj.obj.sclust ? dj.obj.sclust : v->root;
        l.index = off / 32;
        l.lfn_index = dj.blk_ofs != 0xFFFFFFFF ? dj.blk_ofs / 32 : l.index;
        uint64_t lba;
        uint32_t byte;
        uint8_t s[512];
        dir_pos(v, l.dir, l.index, &lba, &byte);
        disk_get(v->d, lba, s);
        l.clust = (uint32_t)(s[byte + 20] | s[byte + 21] << 8) << 16
                | (uint32_t)(s[byte + 26] | s[byte + 27] << 8);
        l.size = rd32(s + byte + 28);
        break;
    }
    f_closedir(&dj);
    return l;
}

// The clusters of a chain, in order (at most `max`); how many.
static uint32_t chain_of(const vol_t *v, uint32_t c, uint32_t *out, uint32_t max) {
    uint32_t n = 0;
    while (n < max && c >= 2 && c < v->count + 2) {
        out[n++] = c;
        c = fat_rd(v, c);
    }
    return n;
}

// --- the base image -------------------------------------------------------------------------

// 512 MB, 4 KB clusters (about 131000 of them), MBR, one FAT, as `sd format` makes it apart
// from the cluster size (f_mkfs would choose 1 sector on so small a volume; 8 sectors also
// takes the paths where a cluster has several sectors).
static vol_t base;
static struct {
    loc_t readme, notes, along, dir, sub, cafe, f1, empty, emptydir, messages, many, n002;
} at;
static uint32_t base_free;          // FSInfo's free count, true for the base image

static void fill_tree(void) {
    put_file("/README.TXT", 100);
    put_file("/notes.txt", 5000);
    put_file("/A long file name.txt", 10000);
    mkdir_ok("/dir");
    mkdir_ok("/dir/sub");
    put_file("/dir/sub/deep.bin", 20000);
    put_file("/dir/sub/café ünïcode.txt", 3000);
    put_file("/dir/f1.txt", 4096);
    put_file("/dir/f2.txt", 8193);
    put_file("/dir/empty.txt", 0);
    mkdir_ok("/dir/emptydir");
    mkdir_ok("/var");
    mkdir_ok("/var/log");
    put_file("/var/log/messages", 70000);
    mkdir_ok("/many");
    for (int i = 0; i < 150; i++) {           // a directory of several clusters
        char name[64];
        snprintf(name, sizeof(name), "/many/Long name number %03d.txt", i);
        put_file(name, 1);
    }
}

static void make_base(void) {
    base = make_volume(1u << 20, FM_FAT32, 1, 4096);
    fill_tree();
    at.readme = locate(&base, "/", "README.TXT");
    at.notes = locate(&base, "/", "notes.txt");
    at.along = locate(&base, "/", "A long file name.txt");
    at.dir = locate(&base, "/", "dir");
    at.sub = locate(&base, "/dir", "sub");
    at.cafe = locate(&base, "/dir/sub", "café ünïcode.txt");
    at.f1 = locate(&base, "/dir", "f1.txt");
    at.empty = locate(&base, "/dir", "empty.txt");
    at.emptydir = locate(&base, "/dir", "emptydir");
    at.messages = locate(&base, "/var/log", "messages");
    at.many = locate(&base, "/", "many");
    at.n002 = locate(&base, "/many", "Long name number 002.txt");
    unmount();
    uint8_t s[512];
    disk_get(base.d, base.vol + 1, s);
    base_free = rd32(s + 488);
}

static vol_t copy_base(void) {
    vol_t v = base;
    v.d = disk_copy(base.d);
    return v;
}

// --- running the checker --------------------------------------------------------------------

#define NO_FAIL UINT64_MAX
#define LINES   64

typedef struct {
    disk_t *d;
    uint64_t fail_lba;          // the read of this sector fails
    uint32_t reads;
    int status;
    fsck_result_t res;
    unsigned n;
    char lines[LINES][FSCK_LINE_MAX];
} run_t;

static uint8_t mem[65536];
static bool subset_upcase;          // pass no upcase callback: fsck_upcase()'s subset

static bool read_cb(void *ctx, uint64_t lba, uint32_t count, uint8_t *buf) {
    run_t *r = ctx;
    r->reads += count;
    for (uint32_t i = 0; i < count; i++) {
        if (lba + i == r->fail_lba || lba + i >= r->d->sectors)
            return false;
        disk_get(r->d, lba + i, buf + 512 * i);
    }
    return true;
}

static void report_cb(void *ctx, const char *line) {
    run_t *r = ctx;
    CHECK(strlen(line) < FSCK_LINE_MAX);
    if (r->n < LINES)
        snprintf(r->lines[r->n++], FSCK_LINE_MAX, "%s", line);
}

static int cmp_line(const void *a, const void *b) {
    return strcmp(a, b);
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void run_check(run_t *r, disk_t *d, uint64_t sectors, size_t window, uint32_t budget,
                      uint64_t fail_lba) {
    memset(r, 0, sizeof(*r));
    r->d = d;
    r->fail_lba = fail_lba;
    fsck_io_t io = {read_cb, report_cb, NULL, r, sectors ? sectors : d->sectors,
                    subset_upcase ? NULL : ff_wtoupper};
    fsck_t *f = fsck_new(&io, mem, fsck_state_size() + window);
    CHECK(f != NULL);
    if (!f)
        return;
    for (uint64_t steps = 0;; steps++) {
        uint32_t before = r->reads;
        r->status = fsck_step(f, budget);
        test_checks++;
        if (r->reads - before > budget) {
            test_failures++;
            fprintf(stderr, "%s: a step read %u sectors, budget %u\n", test_current,
                    r->reads - before, budget);
        }
        if (r->status != FSCK_MORE || steps > 50000000) {
            CHECK(r->status != FSCK_MORE);
            break;
        }
    }
    r->res = *fsck_result(f);
    CHECK_EQ(fsck_step(f, 1), r->status);       // finished stays finished
    qsort(r->lines, r->n, sizeof(r->lines[0]), cmp_line);
}

// printf into one of a few rotating buffers, for expected lines.
__attribute__((format(printf, 1, 2)))
static const char *fmt(const char *format, ...) {
    static char pool[16][FSCK_LINE_MAX];
    static unsigned next;
    char *s = pool[next++ % 16];
    va_list ap;
    va_start(ap, format);
    vsnprintf(s, FSCK_LINE_MAX, format, ap);
    va_end(ap);
    return s;
}

static void show(const char *what, const run_t *r) {
    fprintf(stderr, "  %s (%s, %u passes):\n", what,
            r->status == FSCK_DONE ? r->res.summary : r->res.why ? r->res.why : "?",
            r->res.passes);
    for (unsigned i = 0; i < r->n; i++)
        fprintf(stderr, "    %s\n", r->lines[i]);
}

static run_t big, tiny;

// Check `d` twice, with the board's 16 KB window and a budget of 100 reads, and with a window
// of `tiny_window` bytes and one read per step. Both must give exactly the lines in `want`
// (NULL-terminated, any order) and end with `status`, and then `last` is the summary
// (FSCK_DONE) or the reason (FSCK_FAILED).
static void expect_full(disk_t *d, uint64_t sectors, size_t tiny_window, uint64_t fail_lba,
                        const char *const *want, int status, const char *last) {
    const char *sorted[LINES];
    unsigned nwant = 0;
    while (want[nwant] && nwant < LINES)
        sorted[nwant] = want[nwant], nwant++;
    qsort(sorted, nwant, sizeof(sorted[0]), cmp_str);
    run_check(&big, d, sectors, 16384, 100, fail_lba);
    run_check(&tiny, d, sectors, tiny_window, 1, fail_lba);
    const run_t *runs[2] = {&big, &tiny};
    for (int k = 0; k < 2; k++) {
        const run_t *r = runs[k];
        bool ok = r->status == status && r->n == nwant;
        for (unsigned i = 0; ok && i < nwant; i++)
            ok = !strcmp(r->lines[i], sorted[i]);
        const char *got = r->status == FSCK_DONE ? r->res.summary : r->res.why;
        ok = ok && got && !strcmp(got, last);
        test_checks++;
        if (!ok) {
            test_failures++;
            fprintf(stderr, "%s: %s window: wrong result\n  want (%s):\n", test_current,
                    k ? "tiny" : "16 KB", last);
            for (unsigned i = 0; i < nwant; i++)
                fprintf(stderr, "    %s\n", sorted[i]);
            show("got", r);
        }
    }
    CHECK_EQ(tiny.res.problems, big.res.problems);
    CHECK_EQ(tiny.res.notes, big.res.notes);
    CHECK_EQ(tiny.res.long_chains, big.res.long_chains);
    CHECK_EQ(tiny.res.reached_free, big.res.reached_free);
    CHECK_EQ(tiny.res.lost_clusters, big.res.lost_clusters);
    CHECK_EQ(tiny.res.cross_linked, big.res.cross_linked);
    CHECK_EQ(tiny.res.free_clusters, big.res.free_clusters);
    CHECK_EQ(tiny.res.bad_clusters, big.res.bad_clusters);
    CHECK_EQ(tiny.res.files, big.res.files);
    CHECK_EQ(tiny.res.dirs, big.res.dirs);
    if (status == FSCK_DONE)
        CHECK(tiny.res.passes > big.res.passes);
}

#define LINES_OF(...) ((const char *const[]){__VA_ARGS__, NULL})
// Expect these lines (none: pass NULL) and this summary.
#define EXPECT(disk, summary, ...) \
    expect_full((disk), 0, 64, NO_FAIL, LINES_OF(__VA_ARGS__), FSCK_DONE, (summary))
#define EXPECT_FAIL(disk, why) \
    expect_full((disk), 0, 64, NO_FAIL, LINES_OF(NULL), FSCK_FAILED, (why))

static const char *fsinfo_note(uint32_t counted) {
    return fmt("note: the FSInfo free count is %u, counted %u", base_free, counted);
}

// --- clean volumes --------------------------------------------------------------------------

TEST(clean_mbr) {
    vol_t v = copy_base();
    EXPECT(v.d, "no problems", NULL);
    CHECK_EQ(big.res.kind, FSCK_KIND_FAT32);
    CHECK_EQ(big.res.volume_lba, base.vol);
    CHECK(base.vol > 0);
    CHECK_EQ(big.res.clusters, base.count);
    CHECK_EQ(big.res.cluster_bytes, 4096);
    CHECK_EQ(big.res.free_clusters, base_free);
    CHECK_EQ(big.res.long_chains, 0);
    CHECK_EQ(big.res.files, 159);                 // 9 + 150 in /many
    CHECK_EQ(big.res.dirs, 7);                    // the root, dir, sub, emptydir, var, log, many
    CHECK_EQ(big.res.passes, (base.count - 1) / (16384 * 8) + 1);
    CHECK_EQ(tiny.res.passes, (base.count - 1) / (64 * 8) + 1);
    disk_free(v.d);
}

TEST(clean_no_partition_table) {
    vol_t v = make_volume(1u << 20, FM_FAT32 | FM_SFD, 1, 4096);
    fill_tree();
    unmount();
    EXPECT(v.d, "no problems", NULL);
    CHECK_EQ(big.res.volume_lba, 0);
    disk_free(v.d);
}

TEST(clean_two_fats) {
    vol_t v = make_volume(1u << 20, FM_FAT32, 2, 4096);
    fill_tree();
    unmount();
    CHECK_EQ(v.nfats, 2);
    EXPECT(v.d, "no problems", NULL);
    disk_free(v.d);
}

// f_mkfs's own choices on a small volume: 1-sector clusters.
TEST(clean_one_sector_clusters) {
    vol_t v = make_volume(1u << 17, FM_FAT32, 1, 0);
    fill_tree();
    unmount();
    CHECK_EQ(v.csize, 1);
    EXPECT(v.d, "no problems", NULL);
    disk_free(v.d);
}

// 128 GB: f_mkfs makes a GPT (FF_MIN_GPT) and 32 KB clusters, as on a big card.
static vol_t gpt;

TEST(clean_gpt) {
    gpt = make_volume(0x10000000, FM_FAT32, 1, 0);
    put_file("/README.TXT", 100);
    mkdir_ok("/dir");
    put_file("/dir/A long name in a GPT volume.bin", 100000);
    unmount();
    uint8_t s[512];
    disk_get(gpt.d, 0, s);
    CHECK_EQ(s[446 + 4], 0xEE);                   // a protective MBR
    CHECK_EQ(gpt.csize, 64);
    expect_full(gpt.d, 0, 4096, NO_FAIL, LINES_OF(NULL), FSCK_DONE, "no problems");
    CHECK_EQ(big.res.volume_lba, gpt.vol);
}

TEST(gpt_header_crc_wrong) {
    vol_t v = gpt;
    v.d = disk_copy(gpt.d);
    uint8_t s[512];
    disk_get(v.d, 1, s);
    s[40] ^= 1;                                   // the first usable LBA, under the header CRC
    disk_put(v.d, 1, s);
    expect_full(v.d, 0, 4096, NO_FAIL, LINES_OF(NULL), FSCK_FAILED, "no FAT32 or exFAT volume");
    disk_free(v.d);
}

TEST(gpt_entry_array_crc_wrong) {
    vol_t v = gpt;
    v.d = disk_copy(gpt.d);
    uint8_t b = 1;
    sector_patch(&v, 2, 3 * 128 + 40, &b, 1);     // an unused entry: FatFs doesn't care
    expect_full(v.d, 0, 4096, NO_FAIL, LINES_OF("GPT: the partition entry array's CRC is wrong"),
                FSCK_DONE, "1 problem; 0 bytes lost, 0 cross-linked");
    disk_free(v.d);
    disk_free(gpt.d);
}

TEST(fat16_refused) {
    disk_t *d = disk_new(1u << 16);
    cur = d;
    MKFS_PARM opt = {.fmt = FM_FAT, .n_fat = 1, .align = 8192};
    CHECK_EQ(f_mkfs("0:", &opt, work, sizeof(work)), FR_OK);
    cur = NULL;
    EXPECT_FAIL(d, "not FAT32 or exFAT");
    disk_free(d);
}

// --- finding the volume and refusing boot sectors -------------------------------------------

TEST(no_volume) {
    vol_t v = copy_base();
    uint8_t zero[512] = {0};
    disk_put(v.d, 0, zero);
    EXPECT_FAIL(v.d, "no FAT32 or exFAT volume");
    disk_free(v.d);
}

// Sector 0 without 0x55AA: FatFs doesn't look at the partition table at all.
TEST(mbr_without_signature) {
    vol_t v = copy_base();
    uint8_t z[2] = {0, 0};
    sector_patch(&v, 0, 510, z, 2);
    EXPECT_FAIL(v.d, "no FAT32 or exFAT volume");
    disk_free(v.d);
}

// The volume in the second entry, the first pointing at a sector that isn't a VBR.
TEST(mbr_second_entry) {
    vol_t v = copy_base();
    uint8_t s[512];
    disk_get(v.d, 0, s);
    memcpy(s + 446 + 16, s + 446, 16);
    wr32(s + 446 + 8, 1);
    disk_put(v.d, 0, s);
    EXPECT(v.d, "no problems", NULL);
    disk_free(v.d);
}

static void refused_with(void (*damage)(vol_t *), const char *why) {
    vol_t v = copy_base();
    damage(&v);
    EXPECT_FAIL(v.d, why);
    disk_free(v.d);
}

static void bpb_byte(vol_t *v, uint32_t ofs, uint8_t b) {
    sector_patch(v, v->vol, ofs, &b, 1);
}

static void bad_sector_size(vol_t *v) { bpb_byte(v, 12, 4); }          // 1024
static void bad_nfats(vol_t *v) { bpb_byte(v, 16, 3); }
static void bad_spc(vol_t *v) { bpb_byte(v, 13, 3); }
static void bad_rsv(vol_t *v) { bpb_byte(v, 14, 0); bpb_byte(v, 15, 0); }
static void bad_fsver(vol_t *v) { bpb_byte(v, 42, 1); }
static void bad_fatsize(vol_t *v) {
    uint8_t b[4];
    wr32(b, 100);
    sector_patch(v, v->vol, 36, b, 4);
}
static void bad_root(vol_t *v) {
    uint8_t b[4];
    wr32(b, v->count + 2);
    sector_patch(v, v->vol, 44, b, 4);
}

TEST(boot_sector_refusals) {
    refused_with(bad_sector_size, "bytes per sector not 512");
    refused_with(bad_nfats, "number of FATs not 1 or 2");
    refused_with(bad_spc, "sectors per cluster 0 or not a power of two");
    refused_with(bad_rsv, "reserved sectors 0");
    refused_with(bad_fsver, "FAT32 version not 0.0");
    refused_with(bad_fatsize, "FAT too small for the cluster count");
    refused_with(bad_root, "root directory cluster out of range");
}

TEST(volume_larger_than_device) {
    vol_t v = copy_base();
    expect_full(v.d, v.d->sectors - 1, 64, NO_FAIL, LINES_OF(NULL), FSCK_FAILED,
                "volume larger than the device");
    disk_free(v.d);
}

TEST(read_error) {
    vol_t v = copy_base();
    uint64_t lba = clus_lba(&v, at.dir.clust);
    expect_full(v.d, 0, 64, lba, LINES_OF(NULL), FSCK_FAILED,
                fmt("read error at sector %llu", (unsigned long long)lba));
    disk_free(v.d);
}

// --- the boot region ------------------------------------------------------------------------

TEST(backup_boot_sector_differs) {
    vol_t v = copy_base();
    uint8_t b = 'X';
    sector_patch(&v, v.vol + 6, 3, &b, 1);
    EXPECT(v.d, "1 problem; 0 bytes lost, 0 cross-linked",
           "the backup boot sector (sector 6) differs from the boot sector");
    disk_free(v.d);
}

TEST(fsinfo_signature_wrong) {
    vol_t v = copy_base();
    uint8_t b = 0;
    sector_patch(&v, v.vol + 1, 0, &b, 1);
    EXPECT(v.d, "1 problem; 0 bytes lost, 0 cross-linked",
           "the FSInfo sector's signatures are wrong");
    disk_free(v.d);
}

TEST(fsinfo_free_count_stale) {
    vol_t v = copy_base();
    uint8_t b[4];
    wr32(b, base_free + 5);
    sector_patch(&v, v.vol + 1, 488, b, 4);
    EXPECT(v.d, "no problems", fmt("note: the FSInfo free count is %u, counted %u", base_free + 5,
                                   base_free));
    disk_free(v.d);
}

TEST(media_byte_wrong) {
    vol_t v = copy_base();
    uint8_t b = 0xF0;
    sector_patch(&v, v.fatbase, 0, &b, 1);
    EXPECT(v.d, "no problems", "note: FAT entry 0 doesn't match the media byte (F0, boot sector F8)");
    disk_free(v.d);
}

TEST(dirty_and_error_bits) {
    vol_t v = copy_base();
    fat_wr(&v, 1, 0xF3FFFFFF);
    EXPECT(v.d, "no problems; volume flagged dirty",
           "note: the volume is flagged dirty (not unmounted cleanly)",
           "note: the volume is flagged as having had a disk error");
    CHECK(big.res.dirty);
    disk_free(v.d);
}

TEST(fat_copies_differ) {
    vol_t v = make_volume(1u << 20, FM_FAT32, 2, 4096);
    fill_tree();
    unmount();
    uint8_t b = 0x5A;
    sector_patch(&v, v.fatbase + v.fsize + 3, 17, &b, 1);
    EXPECT(v.d, "1 problem; 0 bytes lost, 0 cross-linked",
           "the two FATs differ in 1 sector (first: FAT sector 3)");
    disk_free(v.d);
}

// --- chains ---------------------------------------------------------------------------------

// A chain nothing reaches, across a boundary of the tiny window (512 clusters a pass): still
// one run.
TEST(lost_chain) {
    vol_t v = copy_base();
    uint32_t x = 2 + 512 * 100 - 1;
    CHECK(!fat_rd(&v, x) && !fat_rd(&v, x + 1) && !fat_rd(&v, x + 2));
    fat_wr(&v, x, x + 1);
    fat_wr(&v, x + 1, x + 2);
    fat_wr(&v, x + 2, 0x0FFFFFFF);
    EXPECT(v.d, "1 problem; 12 KB lost, 0 cross-linked",
           fmt("lost: clusters %u-%u (12 KB)", x, x + 2), fsinfo_note(base_free - 3));
    CHECK_EQ(big.res.lost_clusters, 3);
    disk_free(v.d);
}

// Two entries starting at the same cluster: the second is cross-linked, its own cluster lost.
TEST(cross_link_same_start) {
    vol_t v = copy_base();
    entry_set_clust(&v, &at.f1, at.readme.clust);
    EXPECT(v.d, "2 problems; 4 KB lost, 1 cross-linked",
           fmt("/dir/f1.txt: cross-linked with an earlier entry (both end at cluster %u)",
               at.readme.clust),
           fmt("lost: cluster %u (4 KB)", at.f1.clust));
    disk_free(v.d);
}

// A chain running on into another file's: it shares that file's last 9 clusters.
TEST(cross_link_into_another_chain) {
    vol_t v = copy_base();
    uint32_t a[3], m[18];
    CHECK_EQ(chain_of(&v, at.along.clust, a, 3), 3);
    CHECK_EQ(chain_of(&v, at.messages.clust, m, 18), 18);
    fat_wr(&v, a[2], m[9]);
    EXPECT(v.d, "1 problem; 0 bytes lost, 9 cross-linked",
           "note: /A long file name.txt: 3 clusters for 10000 bytes, chain has 12",
           fmt("/var/log/messages: cross-linked with an earlier entry (both end at cluster %u)",
               m[17]));
    CHECK_EQ(big.res.long_chains, 1);
    disk_free(v.d);
}

// The same across windows: a 3 MB file spans many of the tiny window's (512 clusters each),
// and another chain runs into it. The cross-linked clusters are counted window by window, and
// the line is printed once, in the pass that holds the shared last cluster.
TEST(cross_link_across_windows) {
    vol_t v = copy_base();
    remount(&v);
    put_file("/big.bin", 3u << 20);
    loc_t big_at = locate(&v, "/", "big.bin");
    unmount();
    static uint32_t b[768];
    uint32_t n[2];
    CHECK_EQ(chain_of(&v, big_at.clust, b, 768), 768);
    CHECK_EQ(chain_of(&v, at.notes.clust, n, 2), 2);
    fat_wr(&v, n[1], b[300]);
    EXPECT(v.d, "1 problem; 0 bytes lost, 468 cross-linked",
           "note: /notes.txt: 2 clusters for 5000 bytes, chain has 470",
           fmt("/big.bin: cross-linked with an earlier entry (both end at cluster %u)", b[767]));
    CHECK(b[767] / 512 != b[300] / 512);          // the shared run crosses tiny windows
    disk_free(v.d);
}

TEST(size_larger_than_chain) {
    vol_t v = copy_base();
    entry_set32(&v, at.notes.dir, at.notes.index, 28, 9000);
    EXPECT(v.d, "1 problem; 0 bytes lost, 0 cross-linked",
           "/notes.txt: 3 clusters for 9000 bytes, chain has 2");
    disk_free(v.d);
}

TEST(size_smaller_than_chain) {
    vol_t v = copy_base();
    entry_set32(&v, at.notes.dir, at.notes.index, 28, 100);
    EXPECT(v.d, "no problems", "note: /notes.txt: 1 cluster for 100 bytes, chain has 2");
    CHECK_EQ(big.res.long_chains, 1);
    CHECK_EQ(big.res.notes, 1);
    disk_free(v.d);
}

TEST(size_of_unicode_name) {
    vol_t v = copy_base();
    entry_set32(&v, at.cafe.dir, at.cafe.index, 28, 9000);
    EXPECT(v.d, "1 problem; 0 bytes lost, 0 cross-linked",
           "/dir/sub/café ünïcode.txt: 3 clusters for 9000 bytes, chain has 1");
    disk_free(v.d);
}

TEST(size_without_clusters) {
    vol_t v = copy_base();
    CHECK_EQ(at.empty.clust, 0);
    entry_set32(&v, at.empty.dir, at.empty.index, 28, 100);
    EXPECT(v.d, "1 problem; 0 bytes lost, 0 cross-linked",
           "/dir/empty.txt: 1 cluster for 100 bytes, chain has 0");
    disk_free(v.d);
}

TEST(chain_into_free_cluster) {
    vol_t v = copy_base();
    uint32_t a[3];
    CHECK_EQ(chain_of(&v, at.along.clust, a, 3), 3);
    CHECK_EQ(a[2], a[1] + 1);
    uint32_t free_c = 60000;
    CHECK_EQ(fat_rd(&v, free_c), 0);
    fat_wr(&v, a[0], free_c);
    EXPECT(v.d, "2 problems; 8 KB lost, 0 cross-linked",
           fmt("/A long file name.txt: chain reaches free cluster %u", free_c),
           fmt("lost: clusters %u-%u (8 KB)", a[1], a[2]));
    disk_free(v.d);
}

TEST(chain_loops) {
    vol_t v = copy_base();
    uint32_t a[3];
    chain_of(&v, at.along.clust, a, 3);
    fat_wr(&v, a[2], a[0]);
    EXPECT(v.d, "1 problem; 0 bytes lost, 0 cross-linked", "/A long file name.txt: chain loops");
    disk_free(v.d);
}

// A loop that starts past the file's own length: found by Brent's method.
TEST(long_chain_loops) {
    vol_t v = copy_base();
    uint32_t m[18];
    chain_of(&v, at.messages.clust, m, 18);
    fat_wr(&v, m[17], m[4]);
    EXPECT(v.d, "1 problem; 0 bytes lost, 0 cross-linked", "/var/log/messages: chain loops");
    disk_free(v.d);
}

TEST(bad_cluster_number) {
    vol_t v = copy_base();
    uint32_t a[3];
    chain_of(&v, at.along.clust, a, 3);
    fat_wr(&v, a[0], v.count + 100);
    EXPECT(v.d, "2 problems; 8 KB lost, 0 cross-linked",
           fmt("/A long file name.txt: bad cluster number %u after cluster %u", v.count + 100,
               a[0]),
           fmt("lost: clusters %u-%u (8 KB)", a[1], a[2]));
    disk_free(v.d);
}

TEST(bad_start_cluster) {
    vol_t v = copy_base();
    uint32_t a[3];
    chain_of(&v, at.along.clust, a, 3);
    entry_set_clust(&v, &at.along, 0x0FFFFFF0);
    EXPECT(v.d, "2 problems; 12 KB lost, 0 cross-linked",
           "/A long file name.txt: bad start cluster 268435440",
           fmt("lost: clusters %u-%u (12 KB)", a[0], a[2]));
    disk_free(v.d);
}

TEST(chain_reaches_bad_cluster) {
    vol_t v = copy_base();
    uint32_t a[3];
    chain_of(&v, at.along.clust, a, 3);
    fat_wr(&v, a[1], 0x0FFFFFF7);
    EXPECT(v.d, "2 problems; 4 KB lost, 0 cross-linked; 1 bad cluster",
           fmt("/A long file name.txt: chain reaches bad cluster %u", a[1]),
           fmt("lost: cluster %u (4 KB)", a[2]));
    disk_free(v.d);
}

// A free cluster marked bad: counted, never lost, not a problem.
TEST(bad_cluster_is_not_lost) {
    vol_t v = copy_base();
    fat_wr(&v, 70000, 0x0FFFFFF7);
    EXPECT(v.d, "no problems; 1 bad cluster", fsinfo_note(base_free - 1));
    CHECK_EQ(big.res.bad_clusters, 1);
    disk_free(v.d);
}

// --- directories ----------------------------------------------------------------------------

TEST(directory_loops_to_ancestor) {
    vol_t v = copy_base();
    entry_set_clust(&v, &at.emptydir, at.dir.clust);
    EXPECT(v.d, "2 problems; 4 KB lost, 0 cross-linked",
           "/dir/emptydir: directory loops back to /dir",
           fmt("lost: cluster %u (4 KB)", at.emptydir.clust));
    disk_free(v.d);
}

// Start cluster 0 is the root to FatFs.
TEST(directory_loops_to_root) {
    vol_t v = copy_base();
    entry_set_clust(&v, &at.emptydir, 0);
    EXPECT(v.d, "2 problems; 4 KB lost, 0 cross-linked", "/dir/emptydir: directory loops back to /",
           fmt("lost: cluster %u (4 KB)", at.emptydir.clust));
    disk_free(v.d);
}

TEST(dot_wrong) {
    vol_t v = copy_base();
    uint8_t b[2];
    wr16(b, 12345);
    entry_patch(&v, at.sub.clust, 0, 26, b, 2);
    EXPECT(v.d, "1 problem; 0 bytes lost, 0 cross-linked",
           fmt("/dir/sub: '.' points at cluster 12345, not %u", at.sub.clust));
    disk_free(v.d);
}

TEST(dotdot_wrong) {
    vol_t v = copy_base();
    uint8_t b[2];
    wr16(b, 12345);
    entry_patch(&v, at.sub.clust, 1, 26, b, 2);
    EXPECT(v.d, "1 problem; 0 bytes lost, 0 cross-linked",
           fmt("/dir/sub: '..' points at cluster 12345, not %u", at.dir.clust));
    disk_free(v.d);
}

// A child of the root may name the root's own cluster in "..", as some systems write it.
TEST(dotdot_root_cluster_accepted) {
    vol_t v = copy_base();
    uint8_t b[2];
    wr16(b, v.root);
    entry_patch(&v, at.dir.clust, 1, 26, b, 2);
    EXPECT(v.d, "no problems", NULL);
    disk_free(v.d);
}

TEST(dot_missing) {
    vol_t v = copy_base();
    uint8_t b = 0xE5;
    entry_patch(&v, at.sub.clust, 0, 0, &b, 1);
    EXPECT(v.d, "1 problem; 0 bytes lost, 0 cross-linked", "/dir/sub: no '.' entry");
    disk_free(v.d);
}

TEST(dot_in_root) {
    vol_t v = copy_base();
    entry_patch(&v, at.readme.dir, at.readme.index, 0, ".          ", 11);
    EXPECT(v.d, "2 problems; 4 KB lost, 0 cross-linked",
           fmt("/: '.' entry where it doesn't belong (entry %u)", at.readme.index),
           fmt("lost: cluster %u (4 KB)", at.readme.clust));
    disk_free(v.d);
}

// Deeper than FSCK_DEPTH_MAX: reported, its chain counted, its entries not read.
TEST(nested_too_deep) {
    vol_t v = copy_base();
    remount(&v);
    char p[256] = "";
    for (int i = 0; i < 65; i++) {
        strcat(p, "/n");
        mkdir_ok(p);
    }
    p[2 * 64] = 0;
    loc_t last = locate(&v, p, "n");              // the 65th
    unmount();
    EXPECT(v.d, "2 problems; 4 KB lost, 0 cross-linked",
           fmt("%s: nested more than 63 levels deep: not read", p),
           fmt("lost: cluster %u (4 KB)", last.clust));
    disk_free(v.d);
}

// A path longer than the checker keeps ends in "...".
TEST(long_path_cut) {
    vol_t v = copy_base();
    remount(&v);
    char p[400] = "", name[101];
    memset(name, 'a', 100);
    name[100] = 0;
    for (int i = 0; i < 3; i++) {
        strcat(p, "/");
        strcat(p, name);
        mkdir_ok(p);
    }
    strcat(p, "/f.txt");
    put_file(p, 10);
    p[strlen(p) - 6] = 0;
    loc_t f = locate(&v, p, "f.txt");
    unmount();
    entry_set32(&v, f.dir, f.index, 28, 5000);
    run_check(&big, v.d, 0, 16384, 100, NO_FAIL);
    CHECK_EQ(big.n, 1);
    CHECK_EQ(big.res.problems, 1);
    char want[400];
    snprintf(want, sizeof(want), "/%s/%s/", name, name);
    CHECK(!strncmp(big.lines[0], want, strlen(want)));
    CHECK(strstr(big.lines[0], "aaa...: 2 clusters for 5000 bytes, chain has 1") != NULL);
    disk_free(v.d);
}

// --- names ----------------------------------------------------------------------------------

TEST(orphaned_long_name) {
    vol_t v = copy_base();
    uint8_t b = 0x42;                             // the first stored entry's checksum
    CHECK_EQ(at.along.index - at.along.lfn_index, 2);
    entry_patch(&v, at.along.dir, at.along.lfn_index, 13, &b, 1);
    EXPECT(v.d, "1 problem; 0 bytes lost, 0 cross-linked",
           fmt("/: 2 orphaned long-name entries from entry %u", at.along.lfn_index));
    disk_free(v.d);
}

TEST(long_name_bad_character) {
    vol_t v = copy_base();
    uint8_t star[2] = {'*', 0};
    entry_patch(&v, at.along.dir, at.along.lfn_index + 1, 3, star, 2);   // character 1: ' '
    EXPECT(v.d, "1 problem; 0 bytes lost, 0 cross-linked",
           "/A*long file name.txt: name has characters FatFs refuses");
    disk_free(v.d);
}

TEST(short_name_bad_character) {
    vol_t v = copy_base();
    entry_patch(&v, at.readme.dir, at.readme.index, 4, "*", 1);
    EXPECT(v.d, "1 problem; 0 bytes lost, 0 cross-linked",
           "/READ*E.TXT: name has characters FatFs refuses");
    disk_free(v.d);
}

// FatFs skips a name starting with '.', so its clusters are lost too.
TEST(short_name_starting_with_dot) {
    vol_t v = copy_base();
    entry_patch(&v, at.readme.dir, at.readme.index, 0, ".", 1);
    EXPECT(v.d, "2 problems; 4 KB lost, 0 cross-linked",
           "/.EADME.TXT: name has characters FatFs refuses",
           fmt("lost: cluster %u (4 KB)", at.readme.clust));
    disk_free(v.d);
}

TEST(repeated_short_name) {
    vol_t v = copy_base();
    entry_patch(&v, at.notes.dir, at.notes.index, 0, "README  TXT", 11);
    EXPECT(v.d, "1 problem; 0 bytes lost, 0 cross-linked",
           "/readme.txt: name repeated in the directory");
    disk_free(v.d);
}

// "Long name number 002.txt" made "long name number 001.txt": the same name in another case.
TEST(repeated_long_name) {
    vol_t v = copy_base();
    uint8_t one[2] = {'1', 0}, l[2] = {'l', 0};
    entry_patch(&v, at.n002.dir, at.n002.lfn_index, 16, one, 2);       // character 19
    entry_patch(&v, at.n002.dir, at.n002.lfn_index + 1, 1, l, 2);      // character 0
    EXPECT(v.d, "1 problem; 0 bytes lost, 0 cross-linked",
           "/many/long name number 001.txt: name repeated in the directory");
    disk_free(v.d);
}

// FatFs's own image: the case folding agrees with ff_wtoupper() over the ranges it covers.
TEST(upcase_matches_fatfs) {
    unsigned bad = 0;
    for (uint32_t c = 0; c < 0x530; c++) {
        if (c >= 0x180 && c < 0x370)
            continue;
        if (fsck_upcase((uint16_t)c) != ff_wtoupper(c) && bad++ < 5)
            fprintf(stderr, "upcase %04X: %04X, FatFs %04X\n", c, fsck_upcase((uint16_t)c),
                    (unsigned)ff_wtoupper(c));
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(fsck_upcase(0x4E00), 0x4E00);
}

// --- exFAT ----------------------------------------------------------------------------------

// exFAT images as `sd format` makes them (f_mkfs, n_fat 1, align 8192), filled by FatFs, and
// damaged with tests/fuzz/exfat_damage.h over this file's sparse disk.

static const char *const x_long = "A long file name, longer than thirty characters.txt";
static const char *const x_greek = "Ünïcödé ñame — ελληνικά.txt";
static const char *const x_emoji = "emoji \xF0\x9F\x98\x80 name.txt";
static const char *const x_last = "dir/many/file number 59 with a longer name.txt";

static void append_file(const char *path, uint32_t size) {
    FIL fp;
    CHECK_EQ(f_open(&fp, path, FA_WRITE | FA_OPEN_APPEND), FR_OK);
    uint8_t buf[4096];
    for (uint32_t done = 0; done < size;) {
        UINT n = size - done < sizeof(buf) ? size - done : sizeof(buf), wrote;
        memset(buf, 0x5A, n);
        CHECK_EQ(f_write(&fp, buf, n, &wrote), FR_OK);
        done += n;
    }
    CHECK_EQ(f_close(&fp), FR_OK);
}

// The tree of test_fsck_exfat.c: names in several scripts, a NoFatChain file and a
// fragmented (FAT-chained) one, directories three deep and one of two clusters.
static void fill_exfat(void) {
    mkdir_ok("dir");
    mkdir_ok("dir/sub");
    mkdir_ok("dir/sub/deeper");
    mkdir_ok("dir/many");
    append_file("README.txt", 100);
    append_file(x_long, 5000);
    append_file(x_greek, 3000);
    append_file("日本語のファイル.bin", 700);
    append_file(x_emoji, 10);
    append_file("dir/empty.txt", 0);
    append_file("dir/sub/contig.bin", 200000);      // one write: NoFatChain
    append_file("dir/frag.bin", 9000);              // then something after it,
    append_file("dir/other.bin", 9000);
    append_file("dir/frag.bin", 9000);              // then more: a FAT chain
    append_file("dir/sub/deeper/deep.txt", 1);
    for (int i = 0; i < 60; i++) {
        char p[80];
        snprintf(p, sizeof(p), "dir/many/file number %02d with a longer name.txt", i);
        append_file(p, (uint32_t)(i * 100));
    }
}

static vol_t xbase;

static uint8_t *x_sector(void *ctx, uint64_t lba) {
    return disk_sector(ctx, lba, true);
}

// A copy of the exFAT base image, opened for damage.
static disk_t *xcopy(exd_t *x) {
    disk_t *d = disk_copy(xbase.d);
    CHECK(exd_open(x, x_sector, d, xbase.vol, d->sectors));
    return d;
}

static exd_loc_t xfind(const exd_t *x, const char *p) {
    exd_loc_t l;
    memset(&l, 0, sizeof(l));
    bool found = exd_find(x, p, &l);
    if (!found)
        fprintf(stderr, "%s: %s not found\n", test_current, p);
    CHECK(found);
    return l;
}

// The clusters of a FAT chain (at most `max`).
static uint32_t xchain(const exd_t *x, uint32_t c, uint32_t *out, uint32_t max) {
    uint32_t n = 0;
    while (n < max && c >= 2 && c <= x->g.clusters + 1) {
        out[n++] = c;
        c = exd_fat(x, c);
    }
    return n;
}

// A run line as fsck.c prints it, with its size as fmt_size() gives it.
static const char *x_run(const char *what, uint32_t first, uint32_t n, uint32_t cbytes) {
    static const char *const units[] = {"bytes", "KB", "MB", "GB"};
    uint64_t b = (uint64_t)n * cbytes, div = 1;
    unsigned u = 0;
    while (u < 3 && b >= div * 1024)
        div *= 1024, u++;
    char size[64];
    if ((b % div) * 10 / div)
        snprintf(size, sizeof(size), "%llu.%llu %s", (unsigned long long)(b / div),
                 (unsigned long long)((b % div) * 10 / div), units[u]);
    else
        snprintf(size, sizeof(size), "%llu %s", (unsigned long long)(b / div), units[u]);
    if (n == 1)
        return fmt("%s: cluster %u (%s)", what, first, size);
    return fmt("%s: clusters %u-%u (%s)", what, first, first + n - 1, size);
}

TEST(exfat_clean) {
    disk_t *d = disk_copy(xbase.d);
    EXPECT(d, "no problems", NULL);
    CHECK_EQ(big.res.kind, FSCK_KIND_EXFAT);
    CHECK_EQ(big.res.volume_lba, xbase.vol);
    CHECK(xbase.vol > 0);
    CHECK_EQ(big.res.cluster_bytes, 4096);
    CHECK_EQ(big.res.files, 70);
    CHECK_EQ(big.res.dirs, 5);                     // the root, dir, sub, deeper, many
    CHECK(big.res.free_clusters > 0 && big.res.free_clusters < big.res.clusters);
    disk_free(d);
}

TEST(exfat_clean_no_partition_table) {
    vol_t v = make_volume(131072, FM_EXFAT | FM_SFD, 1, 0);
    fill_exfat();
    unmount();
    EXPECT(v.d, "no problems", NULL);
    CHECK_EQ(big.res.volume_lba, 0);
    disk_free(v.d);
}

// 300 MB: 32 KB clusters, as f_mkfs picks them for a bigger card.
TEST(exfat_clean_big_clusters) {
    vol_t v = make_volume(614400, FM_EXFAT | FM_SFD, 1, 0);
    fill_exfat();
    unmount();
    expect_full(v.d, 0, 64, NO_FAIL, LINES_OF(NULL), FSCK_DONE, "no problems");
    CHECK_EQ(big.res.cluster_bytes, 32768);
    disk_free(v.d);
}

// 128 GB: f_mkfs makes a GPT.
TEST(exfat_clean_gpt) {
    vol_t v = make_volume(0x10000000, FM_EXFAT, 1, 0);
    append_file("README.txt", 100);
    mkdir_ok("dir");
    append_file("dir/A long name in a GPT volume.bin", 100000);
    unmount();
    uint8_t s[512];
    disk_get(v.d, 0, s);
    CHECK_EQ(s[446 + 4], 0xEE);
    expect_full(v.d, 0, 4096, NO_FAIL, LINES_OF(NULL), FSCK_DONE, "no problems");
    CHECK_EQ(big.res.volume_lba, v.vol);
    disk_free(v.d);
}

// Without FatFs's ff_wtoupper(), the checker's subset differs from the volume's table: a
// note. The names here are all inside the subset, so their hashes still agree.
TEST(exfat_subset_upcase) {
    disk_t *d = disk_copy(xbase.d);
    subset_upcase = true;
    run_check(&big, d, 0, 16384, 100, NO_FAIL);
    subset_upcase = false;
    CHECK_EQ(big.status, FSCK_DONE);
    CHECK_EQ(big.n, 1);
    CHECK_EQ(big.res.problems, 0);
    CHECK(!strncmp(big.lines[0], "note: the up-case table maps ", 29));
    disk_free(d);
}

TEST(exfat_refused) {
    exd_t x;
    disk_t *d = xcopy(&x);
    exd_vol(&x, 0)[108] = 12;                       // 4096-byte sectors
    EXPECT_FAIL(d, "bytes per sector not 512");
    CHECK_EQ(big.res.kind, FSCK_KIND_EXFAT);
    disk_free(d);
}

TEST(exfat_set_checksum) {
    exd_t x;
    disk_t *d = xcopy(&x);
    exd_loc_t l = xfind(&x, "README.txt");
    exd_bad_set_checksum(&x, &l);
    EXPECT(d, "1 problem; 0 bytes lost, 0 cross-linked",
           fmt("/README.txt: set checksum 0x%04X, computed 0x%04X",
               (uint16_t)(l.set.sum_stored + 1), l.set.sum_stored));
    disk_free(d);
}

TEST(exfat_name_hash) {
    exd_t x;
    disk_t *d = xcopy(&x);
    exd_loc_t l = xfind(&x, x_greek);
    exd_bad_name_hash(&x, &l);
    EXPECT(d, "1 problem; 0 bytes lost, 0 cross-linked",
           fmt("/%s: name hash 0x%04X, computed 0x%04X", x_greek,
               (uint16_t)(l.set.hash_stored + 1), l.set.hash_stored));
    disk_free(d);
}

TEST(exfat_secondary_count) {
    exd_t x;
    disk_t *d = xcopy(&x);
    exd_loc_t l = xfind(&x, x_long);                // a stream and 4 name entries
    exd_secondary_count(&x, &l, -1);
    // The path shows the name as far as the set holds it: 3 entries of 15 characters.
    EXPECT(d, "1 problem; 0 bytes lost, 0 cross-linked",
           "/A long file name, longer than thirty characte: secondary count 4 doesn't fit a "
           "name of 51 characters (needs 5)");
    disk_free(d);

    // Under FatFs's minimum: only the stream entry is in the set, so it has no name.
    d = xcopy(&x);
    l = xfind(&x, "README.txt");
    exd_secondary_count(&x, &l, -1);
    EXPECT(d, "1 problem; 0 bytes lost, 0 cross-linked",
           fmt("/: file entry %u: secondary count 1 (FatFs reads 2 to 18)", l.index));
    disk_free(d);

    // One too many: the next set's File entry cuts it short.
    d = xcopy(&x);
    l = xfind(&x, "README.txt");
    exd_secondary_count(&x, &l, +1);
    EXPECT(d, "1 problem; 0 bytes lost, 0 cross-linked",
           "/README.txt: entry set ends after 2 of 3 secondary entries");
    disk_free(d);
}

TEST(exfat_name_length) {
    exd_t x;
    disk_t *d = xcopy(&x);
    exd_loc_t l = xfind(&x, "dir/sub/deeper/deep.txt");
    exd_name_length(&x, &l, 16);
    EXPECT(d, "1 problem; 0 bytes lost, 0 cross-linked",
           "/dir/sub/deeper/deep.txt: secondary count 2 doesn't fit a name of 16 characters "
           "(needs 3)");
    disk_free(d);
}

TEST(exfat_valid_length) {
    exd_t x;
    disk_t *d = xcopy(&x);
    exd_loc_t l = xfind(&x, "dir/frag.bin");
    exd_valid_over(&x, &l);
    EXPECT(d, "1 problem; 0 bytes lost, 0 cross-linked",
           "/dir/frag.bin: valid data length 18001 is larger than the data length 18000");
    disk_free(d);
}

// The last file written runs past the heap: its clusters up to the heap's end are reached
// but clear in the bitmap.
TEST(exfat_run_past_heap) {
    exd_t x;
    disk_t *d = xcopy(&x);
    exd_loc_t l = xfind(&x, x_last);
    CHECK(l.set.no_fat_chain);
    exd_run_past_heap(&x, &l, 3);
    uint32_t last = x.g.clusters + 1, first = l.set.first_cluster;
    uint32_t n = last - (first + l.set.clusters) + 1;
    EXPECT(d, fmt("2 problems; 0 bytes lost, 0 cross-linked"),
           fmt("/%s: contiguous clusters %u-%u run past the last cluster (%u)", x_last, first,
               last + 3, last),
           x_run("reached but free", first + l.set.clusters, n, 4096));
    CHECK_EQ(big.res.reached_free, n);
    disk_free(d);
}

TEST(exfat_bitmap_entries) {
    exd_t x;
    disk_t *d = xcopy(&x);
    CHECK(exd_no_bitmap(&x));
    EXPECT(d, "1 problem; 0 bytes lost, 0 cross-linked", "/: no allocation bitmap entry");
    disk_free(d);

    d = xcopy(&x);
    uint32_t idx;
    CHECK(exd_second_bitmap(&x, &idx));
    EXPECT(d, "1 problem; 0 bytes lost, 0 cross-linked",
           fmt("/: allocation bitmap entry %u: a second allocation bitmap entry (FatFs uses the "
               "first)", idx));
    disk_free(d);
}

// The bitmap's chain doesn't end in 0xFFFFFFFF: FatFs refuses the volume.
TEST(exfat_bitmap_not_contiguous) {
    exd_t x;
    disk_t *d = xcopy(&x);
    exd_dir_t root = {x.g.root_cluster, false};
    uint32_t i;
    CHECK(exd_find_type(&x, root, 0x81, &i));
    uint32_t bm = exd_le32(exd_entry(&x, root, i) + 20);
    CHECK_EQ(exd_fat(&x, bm), 0xFFFFFFFF);
    exd_set_fat(&x, bm, 0xFFFFFFF8);
    EXPECT(d, "1 problem; 0 bytes lost, 0 cross-linked",
           fmt("the allocation bitmap's chain isn't contiguous at cluster %u (FatFs won't mount "
               "the volume)", bm));
    disk_free(d);
}

TEST(exfat_upcase_checksum) {
    exd_t x;
    disk_t *d = xcopy(&x);
    exd_dir_t root = {x.g.root_cluster, false};
    uint32_t i;
    CHECK(exd_find_type(&x, root, 0x82, &i));
    uint32_t sum = exd_le32(exd_entry(&x, root, i) + 4);
    CHECK(exd_bad_upcase_checksum(&x));
    EXPECT(d, "1 problem; 0 bytes lost, 0 cross-linked",
           fmt("up-case table checksum 0x%08X, computed 0x%08X", sum + 1, sum));
    disk_free(d);
}

TEST(exfat_boot_region) {
    exd_t x;
    disk_t *d = xcopy(&x);
    exd_bad_boot_checksum(&x);
    EXPECT(d, "1 problem; 0 bytes lost, 0 cross-linked", "boot region checksum wrong (sector 11)");
    disk_free(d);

    d = xcopy(&x);
    exd_backup_differs(&x, 14);
    EXPECT(d, "1 problem; 0 bytes lost, 0 cross-linked",
           "backup boot region differs from the main one (sector 14)");
    disk_free(d);

    d = xcopy(&x);
    exd_dirty(&x);
    EXPECT(d, "no problems; volume flagged dirty",
           "note: the volume is flagged dirty (not unmounted cleanly)");
    CHECK(big.res.dirty);
    disk_free(d);
}

// A bit cleared under a file, and bits set for nothing, across a boundary of the tiny window
// (512 clusters a pass): still one run.
TEST(exfat_bitmap_bits) {
    exd_t x;
    disk_t *d = xcopy(&x);
    exd_loc_t l = xfind(&x, "dir/sub/contig.bin");
    uint32_t c = l.set.first_cluster + 10;
    exd_bitmap_bit(&x, c, false);
    EXPECT(d, "1 problem; 0 bytes lost, 0 cross-linked", x_run("reached but free", c, 1, 4096));
    CHECK_EQ(big.res.reached_free, 1);
    disk_free(d);

    d = xcopy(&x);
    uint32_t far = 2 + 512 * 20 - 1;
    for (uint32_t k = 0; k < 3; k++)
        exd_bitmap_bit(&x, far + k, true);
    EXPECT(d, "1 problem; 12 KB lost, 0 cross-linked", x_run("lost", far, 3, 4096));
    CHECK_EQ(big.res.lost_clusters, 3);
    disk_free(d);
}

TEST(exfat_unknown_critical) {
    exd_t x;
    disk_t *d = xcopy(&x);
    exd_loc_t l = xfind(&x, "dir/sub");
    uint32_t idx;
    CHECK(exd_unknown_critical(&x, (exd_dir_t){l.set.first_cluster, l.set.no_fat_chain}, &idx));
    EXPECT(d, "1 problem; 0 bytes lost, 0 cross-linked",
           fmt("/dir/sub: unknown critical entry type 0x84 (entry %u)", idx));
    disk_free(d);
}

// Two NoFatChain files made to share a cluster: the later one in the walk is named.
TEST(exfat_cross_link) {
    exd_t x;
    disk_t *d = xcopy(&x);
    exd_loc_t a = xfind(&x, "dir/sub/contig.bin"), b = xfind(&x, "README.txt");
    CHECK(b.set.no_fat_chain);
    exd_put32(exd_set_entry(&x, &b, 1) + 20, a.set.first_cluster);
    exd_resum(&x, &b);
    EXPECT(d, "2 problems; 4 KB lost, 1 cross-linked",
           fmt("/README.txt: cross-linked with an earlier entry (at cluster %u)",
               a.set.first_cluster),
           x_run("lost", b.set.first_cluster, 1, 4096));
    disk_free(d);
}

// A later file made to start one cluster early, on an earlier file's last: they share a
// cluster, but not the later one's last, so no entry is named; the count is.
TEST(exfat_cross_link_unnamed) {
    exd_t x;
    disk_t *d = xcopy(&x);
    exd_loc_t a = xfind(&x, x_greek), b = xfind(&x, "日本語のファイル.bin");
    CHECK(a.set.no_fat_chain && b.set.no_fat_chain);
    CHECK_EQ(a.set.clusters, 1);
    CHECK_EQ(b.set.first_cluster, a.set.first_cluster + 1);
    uint8_t *s = exd_set_entry(&x, &b, 1);
    exd_put32(s + 20, a.set.first_cluster);
    exd_put64(s + 24, 2 * 4096);
    exd_put64(s + 8, 2 * 4096);
    exd_resum(&x, &b);
    EXPECT(d, "1 problem; 0 bytes lost, 1 cross-linked", "1 cluster used by more than one entry");
    disk_free(d);
}

static void x_set_length(const exd_t *x, const exd_loc_t *l, uint64_t len) {
    uint8_t *s = exd_set_entry(x, l, 1);
    exd_put64(s + 24, len);
    exd_put64(s + 8, len);
    exd_resum(x, l);
}

TEST(exfat_size_against_chain) {
    exd_t x;
    disk_t *d = xcopy(&x);
    exd_loc_t l = xfind(&x, "dir/frag.bin");
    CHECK(!l.set.no_fat_chain);
    x_set_length(&x, &l, 30000);
    EXPECT(d, "1 problem; 0 bytes lost, 0 cross-linked",
           "/dir/frag.bin: 8 clusters for 30000 bytes, chain has 5");
    disk_free(d);

    d = xcopy(&x);
    x_set_length(&x, &l, 5000);
    EXPECT(d, "no problems", "note: /dir/frag.bin: 2 clusters for 5000 bytes, chain has 5");
    CHECK_EQ(big.res.long_chains, 1);
    disk_free(d);
}

TEST(exfat_chain_damage) {
    exd_t x;
    disk_t *d = xcopy(&x);
    exd_loc_t l = xfind(&x, "dir/frag.bin");
    uint32_t c[5];
    CHECK_EQ(xchain(&x, l.set.first_cluster, c, 5), 5);
    exd_set_fat(&x, c[4], c[0]);
    EXPECT(d, "1 problem; 0 bytes lost, 0 cross-linked", "/dir/frag.bin: chain loops");
    disk_free(d);

    // Into a free cluster: the rest of the chain is lost.
    d = xcopy(&x);
    uint32_t free_c = x.g.clusters - 10;
    exd_set_fat(&x, c[0], free_c);
    CHECK_EQ(c[1], c[0] + 1);
    CHECK_EQ(c[2], c[1] + 1);
    CHECK_EQ(c[4], c[3] + 1);
    EXPECT(d, "3 problems; 16 KB lost, 0 cross-linked",
           fmt("/dir/frag.bin: chain reaches free cluster %u", free_c),
           x_run("lost", c[1], 2, 4096), x_run("lost", c[3], 2, 4096));
    disk_free(d);

    d = xcopy(&x);
    exd_set_fat(&x, c[0], x.g.clusters + 100);
    EXPECT(d, "3 problems; 16 KB lost, 0 cross-linked",
           fmt("/dir/frag.bin: bad cluster number %u after cluster %u", x.g.clusters + 100, c[0]),
           x_run("lost", c[1], 2, 4096), x_run("lost", c[3], 2, 4096));
    disk_free(d);
}

TEST(exfat_directory_loop) {
    exd_t x;
    disk_t *d = xcopy(&x);
    exd_loc_t dir = xfind(&x, "dir"), deeper = xfind(&x, "dir/sub/deeper");
    exd_loc_t deep = xfind(&x, "dir/sub/deeper/deep.txt");
    exd_put32(exd_set_entry(&x, &deeper, 1) + 20, dir.set.first_cluster);
    exd_resum(&x, &deeper);
    uint32_t a = deeper.set.first_cluster, b = deep.set.first_cluster;
    CHECK(a + 1 != b && b + 1 != a);
    EXPECT(d, "3 problems; 8 KB lost, 0 cross-linked",
           "/dir/sub/deeper: directory loops back to /dir", x_run("lost", a, 1, 4096),
           x_run("lost", b, 1, 4096));
    disk_free(d);
}

// "file number 01 ..." renamed "FILE number 00 ...": the same name as an earlier one's, in
// another case. Its hash and checksum are made right, so only the repeat shows.
TEST(exfat_repeated_name) {
    exd_t x;
    disk_t *d = xcopy(&x);
    exd_loc_t l = xfind(&x, "dir/many/file number 01 with a longer name.txt");
    const char *nm = "FILE number 00 with a longer name.txt";
    uint16_t u[64];
    size_t n = exd_utf16(nm, nm + strlen(nm), u, 64);
    CHECK_EQ(n, l.set.name_len);
    for (size_t i = 0; i < n; i++)
        exd_put16(exd_set_entry(&x, &l, 2 + (uint32_t)(i / 15)) + 2 + 2 * (i % 15), u[i]);
    exd_put16(exd_set_entry(&x, &l, 1) + 4, fsck_exfat_name_hash(u, n, ff_wtoupper));
    exd_resum(&x, &l);
    EXPECT(d, "1 problem; 0 bytes lost, 0 cross-linked",
           fmt("/dir/many/%s: name repeated in the directory", nm));
    disk_free(d);
}

TEST(exfat_nested_too_deep) {
    vol_t v = make_volume(131072, FM_EXFAT | FM_SFD, 1, 0);
    char p[256] = "";
    for (int i = 0; i < 65; i++) {
        strcat(p, "/n");
        mkdir_ok(p);
    }
    unmount();
    p[2 * 64] = 0;
    exd_t x;
    CHECK(exd_open(&x, x_sector, v.d, 0, v.d->sectors));
    exd_loc_t last = xfind(&x, "n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/"
                               "n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n/n");
    EXPECT(v.d, "2 problems; 4 KB lost, 0 cross-linked",
           fmt("%s: nested more than 63 levels deep: not read", p),
           x_run("lost", last.set.first_cluster, 1, 4096));
    disk_free(v.d);
}

// --- the interface --------------------------------------------------------------------------

TEST(memory) {
    fsck_io_t io = {read_cb, NULL, NULL, NULL, 1000, NULL};
    CHECK(fsck_new(&io, mem, fsck_state_size() + FSCK_WINDOW_MIN - 1) == NULL);
    CHECK(fsck_new(&io, mem + 1, fsck_state_size() + FSCK_WINDOW_MIN + 7) != NULL);
    CHECK(fsck_new(&io, mem + 1, fsck_state_size() + FSCK_WINDOW_MIN + 6) == NULL);
    io.sectors = 0;
    CHECK(fsck_new(&io, mem, sizeof(mem)) == NULL);
    io.sectors = 1000;
    io.read = NULL;
    CHECK(fsck_new(&io, mem, sizeof(mem)) == NULL);
}

// More problems than FSCK_REPORT_MAX: the lines stop, the count doesn't.
TEST(report_limit) {
    vol_t v = copy_base();
    for (uint32_t i = 0; i < 60; i++)             // 60 single lost clusters
        fat_wr(&v, 80000 + 2 * i, 0x0FFFFFFF);
    run_check(&big, v.d, 0, 16384, 100, NO_FAIL);
    CHECK_EQ(big.n, FSCK_REPORT_MAX);
    CHECK_EQ(big.res.reported, FSCK_REPORT_MAX);
    CHECK_EQ(big.res.problems, 60);
    CHECK_EQ(big.res.notes, 1);
    CHECK(!strcmp(big.res.summary, "60 problems; 240 KB lost, 0 cross-linked"));
    disk_free(v.d);
}

// The fuzz seeds (written by seeds.py, not FatFs) are clean volumes too.
TEST(fuzz_seeds) {
    static const char *const names[] = {"mbr", "sfd-2fat", "gpt", "exfat", "exfat-mbr"};
    static fsck_image_t im;
    static uint8_t buf[16384];
    for (int i = 0; i < 5; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/seed-%s", FSCK_SEEDS_DIR, names[i]);
        FILE *fp = fopen(path, "rb");
        CHECK(fp != NULL);
        if (!fp)
            continue;
        size_t n = fread(buf, 1, sizeof(buf), fp);
        fclose(fp);
        CHECK(n > 1 && n < sizeof(buf));
        fsck_image_parse(&im, buf + 1, n - 1);    // after the fuzz target's selector byte
        fsck_io_t io = {fsck_image_read, NULL, NULL, &im, im.sectors, NULL};
        fsck_t *f = fsck_new(&io, mem, fsck_state_size() + 2048);
        int st;
        while ((st = fsck_step(f, 64)) == FSCK_MORE)
            ;
        const fsck_result_t *res = fsck_result(f);
        CHECK_EQ(st, FSCK_DONE);
        CHECK(!strcmp(res->summary, "no problems"));
        CHECK_EQ(res->notes, 0);
        CHECK_EQ(res->files, i < 3 ? 4 : 6);
        CHECK_EQ(res->dirs, 2);
        CHECK_EQ(res->kind, i < 3 ? FSCK_KIND_FAT32 : FSCK_KIND_EXFAT);
        CHECK(res->passes > 1);
    }
}

TEST_MAIN(
    make_base();
    xbase = make_volume(131072, FM_EXFAT, 1, 0);   // 64 MB, with a partition table
    fill_exfat();
    unmount();
    RUN(clean_mbr);
    RUN(clean_no_partition_table);
    RUN(clean_two_fats);
    RUN(clean_one_sector_clusters);
    RUN(clean_gpt);
    RUN(gpt_header_crc_wrong);
    RUN(gpt_entry_array_crc_wrong);
    RUN(fat16_refused);
    RUN(no_volume);
    RUN(mbr_without_signature);
    RUN(mbr_second_entry);
    RUN(boot_sector_refusals);
    RUN(volume_larger_than_device);
    RUN(read_error);
    RUN(backup_boot_sector_differs);
    RUN(fsinfo_signature_wrong);
    RUN(fsinfo_free_count_stale);
    RUN(media_byte_wrong);
    RUN(dirty_and_error_bits);
    RUN(fat_copies_differ);
    RUN(lost_chain);
    RUN(cross_link_same_start);
    RUN(cross_link_into_another_chain);
    RUN(cross_link_across_windows);
    RUN(size_larger_than_chain);
    RUN(size_smaller_than_chain);
    RUN(size_of_unicode_name);
    RUN(size_without_clusters);
    RUN(chain_into_free_cluster);
    RUN(chain_loops);
    RUN(long_chain_loops);
    RUN(bad_cluster_number);
    RUN(bad_start_cluster);
    RUN(chain_reaches_bad_cluster);
    RUN(bad_cluster_is_not_lost);
    RUN(directory_loops_to_ancestor);
    RUN(directory_loops_to_root);
    RUN(dot_wrong);
    RUN(dotdot_wrong);
    RUN(dotdot_root_cluster_accepted);
    RUN(dot_missing);
    RUN(dot_in_root);
    RUN(nested_too_deep);
    RUN(long_path_cut);
    RUN(orphaned_long_name);
    RUN(long_name_bad_character);
    RUN(short_name_bad_character);
    RUN(short_name_starting_with_dot);
    RUN(repeated_short_name);
    RUN(repeated_long_name);
    RUN(upcase_matches_fatfs);
    RUN(exfat_clean);
    RUN(exfat_clean_no_partition_table);
    RUN(exfat_clean_big_clusters);
    RUN(exfat_clean_gpt);
    RUN(exfat_subset_upcase);
    RUN(exfat_refused);
    RUN(exfat_set_checksum);
    RUN(exfat_name_hash);
    RUN(exfat_secondary_count);
    RUN(exfat_name_length);
    RUN(exfat_valid_length);
    RUN(exfat_run_past_heap);
    RUN(exfat_bitmap_entries);
    RUN(exfat_bitmap_not_contiguous);
    RUN(exfat_upcase_checksum);
    RUN(exfat_boot_region);
    RUN(exfat_bitmap_bits);
    RUN(exfat_unknown_critical);
    RUN(exfat_cross_link);
    RUN(exfat_cross_link_unnamed);
    RUN(exfat_size_against_chain);
    RUN(exfat_chain_damage);
    RUN(exfat_directory_loop);
    RUN(exfat_repeated_name);
    RUN(exfat_nested_too_deep);
    RUN(memory);
    RUN(report_limit);
    RUN(fuzz_seeds);
    disk_free(base.d);
    disk_free(xbase.d);
)
