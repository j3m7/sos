// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Self-tests for the card checker's fault-injection harness (tests/fsck_disk.c,
// tests/fsck_faults.c, tests/fsck_damage.c; docs/design/sd-check.md §10.4). Repair doesn't exist
// yet: these hold the harness to what it claims, with toy repairs and host FatFs as the judge.

#define _GNU_SOURCE                 // open_memstream
#include <stdlib.h>

#include "fsck_damage.h"
#include "fsck_disk.h"
#include "fsck_faults.h"
#include "test.h"

#define SECTOR 512u

static void fill(uint8_t *buf, uint8_t v) {
    memset(buf, v, SECTOR);
}

static uint8_t first_byte(const fsck_disk_t *d, uint64_t lba) {
    uint8_t s[SECTOR];
    fsck_disk_get(d, lba, s);
    return s[0];
}

// --- the fault layer ------------------------------------------------------------------------

TEST(fault_layer_counts) {
    fsck_disk_t *d = fsck_disk_new(100);
    fault_disk_t f;
    fault_init(&f, d);
    fault_arm(&f, FAULT_OFF, 0);
    fault_io_t io = fault_io(&f);
    static uint8_t buf[3 * SECTOR];
    memset(buf, 7, sizeof(buf));
    CHECK(io.write(io.ctx, 10, 3, buf));
    CHECK(io.write(io.ctx, 20, 1, buf));
    CHECK(io.write(io.ctx, 10, 1, buf));
    CHECK(io.read(io.ctx, 10, 2, buf));
    CHECK(!io.read(io.ctx, 99, 2, buf));            // past the end
    CHECK(!io.write(io.ctx, 100, 1, buf));
    CHECK_EQ(io.sectors, 100);
    CHECK_EQ(f.writes, 5);
    CHECK_EQ(f.applied, 5);
    CHECK_EQ(f.reads, 4);
    CHECK_EQ(fault_writes_of(&f, 10), 2);
    CHECK_EQ(fault_writes_of(&f, 12), 1);
    CHECK_EQ(fault_writes_of(&f, 13), 0);
    CHECK_EQ(fault_reads_of(&f, 11), 1);
    CHECK_EQ(fault_reads_of(&f, 20), 0);
    CHECK_EQ(f.log_len, 5);
    static const uint64_t order[] = {10, 11, 12, 20, 10};
    for (size_t i = 0; i < 5; i++) {
        CHECK_EQ(f.log[i].lba, order[i]);
        CHECK_EQ(f.log[i].seq, i + 1);
        CHECK(f.log[i].applied);
    }
    CHECK_EQ(first_byte(d, 12), 7);
    CHECK(!f.cut && !f.failed);
    fault_arm(&f, FAULT_OFF, 0);                    // arming clears everything
    CHECK_EQ(f.writes, 0);
    CHECK_EQ(f.log_len, 0);
    CHECK_EQ(fault_writes_of(&f, 10), 0);
    fault_free(&f);
    fsck_disk_free(d);
}

TEST(fault_layer_cut) {
    fsck_disk_t *d = fsck_disk_new(100);
    fault_disk_t f;
    fault_init(&f, d);
    uint8_t buf[SECTOR];
    fault_arm(&f, FAULT_CUT, 2);
    for (uint8_t i = 1; i <= 4; i++) {
        fill(buf, i);
        CHECK(fault_write(&f, i, 1, buf));          // a cut write still reports success
    }
    CHECK_EQ(first_byte(d, 1), 1);
    CHECK_EQ(first_byte(d, 2), 2);
    CHECK_EQ(first_byte(d, 3), 0);
    CHECK_EQ(first_byte(d, 4), 0);
    CHECK(f.cut);
    CHECK_EQ(f.applied, 2);
    CHECK_EQ(f.dropped, 2);
    CHECK(f.log[1].applied && !f.log[2].applied);

    // A call of several sectors is cut in the middle.
    static uint8_t three[3 * SECTOR];
    memset(three, 9, sizeof(three));
    fault_arm(&f, FAULT_CUT, 2);
    CHECK(fault_write(&f, 50, 3, three));
    CHECK_EQ(first_byte(d, 50), 9);
    CHECK_EQ(first_byte(d, 51), 9);
    CHECK_EQ(first_byte(d, 52), 0);

    // Cut after 0: nothing lands; cut after more than are written: no cut.
    fault_arm(&f, FAULT_CUT, 0);
    CHECK(fault_write(&f, 60, 1, buf));
    CHECK_EQ(first_byte(d, 60), 0);
    CHECK(f.cut);
    fault_arm(&f, FAULT_CUT, 5);
    CHECK(fault_write(&f, 61, 1, buf));
    CHECK_EQ(first_byte(d, 61), 4);
    CHECK(!f.cut);
    fault_free(&f);
    fsck_disk_free(d);
}

TEST(fault_layer_fail_write) {
    fsck_disk_t *d = fsck_disk_new(100);
    fault_disk_t f;
    fault_init(&f, d);
    uint8_t buf[SECTOR];
    fault_arm(&f, FAULT_FAIL_WRITE, 2);
    fill(buf, 1);
    CHECK(fault_write(&f, 1, 1, buf));
    fill(buf, 2);
    CHECK(!fault_write(&f, 2, 1, buf));
    CHECK(f.failed);
    CHECK_EQ(f.failed_at, 2);
    CHECK_EQ(first_byte(d, 2), 0);                  // nothing applied
    CHECK_EQ(f.writes_after_failure, 0);
    fill(buf, 3);
    CHECK(!fault_write(&f, 3, 1, buf));             // after the failure: refused, counted
    CHECK_EQ(first_byte(d, 3), 0);
    CHECK_EQ(f.writes_after_failure, 1);
    CHECK(f.log[2].after_failure && !f.log[2].applied);
    CHECK_EQ(first_byte(d, 1), 1);

    // The whole call holding write N writes nothing.
    static uint8_t three[3 * SECTOR];
    memset(three, 9, sizeof(three));
    fault_arm(&f, FAULT_FAIL_WRITE, 2);
    CHECK(!fault_write(&f, 50, 3, three));
    CHECK_EQ(first_byte(d, 50), 0);
    CHECK_EQ(f.writes, 3);
    CHECK_EQ(f.applied, 0);
    fault_free(&f);
    fsck_disk_free(d);
}

TEST(fault_layer_fail_read) {
    fsck_disk_t *d = fsck_disk_new(100);
    fault_disk_t f;
    fault_init(&f, d);
    static uint8_t buf[2 * SECTOR];
    fault_arm(&f, FAULT_FAIL_READ, 3);
    CHECK(fault_read(&f, 1, 1, buf));
    CHECK(!fault_read(&f, 2, 2, buf));              // reads 2 and 3: 3 fails
    CHECK(f.failed);
    CHECK_EQ(f.failed_at, 3);
    CHECK(fault_read(&f, 4, 1, buf));               // later reads work, and are counted
    CHECK_EQ(f.reads_after_failure, 1);
    CHECK(!fault_write(&f, 5, 1, buf));
    CHECK_EQ(f.writes_after_failure, 1);
    CHECK_EQ(first_byte(d, 5), 0);
    fault_free(&f);
    fsck_disk_free(d);
}

// --- the disk and its snapshot --------------------------------------------------------------

static fsck_disk_t *base1, *base2;      // populated FAT32 images: one FAT, two FATs
static fsck_fp_t base1_fp, base2_fp;
static damage_catalog_t base1_cat, base2_cat;

static fsck_disk_t *make_base(BYTE n_fat) {
    fsck_disk_t *d = fsck_disk_fat32(n_fat);
    CHECK(d != NULL);
    if (!d)
        exit(1);
    CHECK(fsck_disk_mount(d));
    CHECK(fsck_disk_populate());
    fsck_disk_unmount();
    return d;
}

TEST(builder) {
    damage_fat32_t g;
    CHECK(damage_fat32_parse(base1, &g));
    CHECK_EQ(g.nfats, 1);
    CHECK_EQ(g.spc, 2);
    CHECK(g.count > 65525);
    CHECK(g.vol > 0);                               // MBR: a partition table
    CHECK_EQ(damage_fsinfo_free(base1, &g), damage_count_free(base1, &g));
    damage_fat32_t g2;
    CHECK(damage_fat32_parse(base2, &g2));
    CHECK_EQ(g2.nfats, 2);
    // FatFs agrees with the parse.
    CHECK(fsck_disk_mount(base2));
    FATFS *fs = fsck_disk_fs();
    CHECK_EQ(fs->n_fats, 2);
    CHECK_EQ(fs->fatbase, g2.fatbase);
    CHECK_EQ(fs->database, g2.database);
    CHECK_EQ(fs->n_fatent - 2, g2.count);
    fsck_disk_unmount();
    // The tree is all there, and reads as written.
    CHECK(base1_fp.n >= 40);
    int i = damage_catalog_find(&base1_cat, "/var/log/messages");
    CHECK(i >= 0);
    if (i >= 0)
        CHECK_EQ(base1_cat.e[i].size, 70000);
    CHECK(damage_catalog_find(&base1_cat, "/dir/sub/caf\xc3\xa9 \xc3\xbcn\xc3\xaf" "code.txt") >= 0);
    CHECK(damage_catalog_find(&base1_cat, "/dir/emptydir") >= 0);
    size_t bad = 0;
    for (size_t k = 0; k < base1_fp.n; k++)
        bad += !base1_fp.e[k].read_ok;
    CHECK_EQ(bad, 0);
}

TEST(snapshot_restore_exact) {
    fsck_disk_t *copy = fsck_disk_copy(base1);
    CHECK(fsck_disk_equal(copy, base1));
    fsck_disk_snapshot(base1);
    uint8_t buf[SECTOR];
    uint64_t rng = 12345;
    for (unsigned round = 0; round < 3; round++) {
        for (unsigned i = 0; i < 500; i++) {
            rng = rng * 6364136223846793005ull + 1442695040888963407ull;
            uint64_t lba = (rng >> 33) % (fsck_disk_sectors(base1) / 64);   // old and new sectors
            fill(buf, (uint8_t)(i | 1));
            fsck_disk_put(base1, lba, buf);
        }
        CHECK(!fsck_disk_equal(copy, base1));
        uint64_t lbas[4];
        CHECK(fsck_disk_changed(base1, lbas, 4) > 0);
        fsck_disk_restore(base1);
        CHECK(fsck_disk_equal(copy, base1));
        CHECK_EQ(fsck_disk_changed(base1, lbas, 4), 0);
    }
    // Writing a sector with what it already holds isn't a change.
    fsck_disk_get(base1, 0, buf);
    fsck_disk_put(base1, 0, buf);
    uint64_t lba;
    CHECK_EQ(fsck_disk_changed(base1, &lba, 1), 0);
    fill(buf, 0x5A);
    fsck_disk_put(base1, 3, buf);
    CHECK_EQ(fsck_disk_changed(base1, &lba, 1), 1);
    CHECK_EQ(lba, 3);
    fsck_disk_restore(base1);
    CHECK(fsck_disk_equal(copy, base1));
    fsck_disk_free(copy);
}

// --- the runner, with toy repairs -----------------------------------------------------------

static const uint64_t toy_seq[] = {100, 101, 102, 100, 103, 104};
#define TOY_N (sizeof(toy_seq) / sizeof(toy_seq[0]))

typedef struct {
    bool nondeterministic;      // every other run writes in another order
    bool keep_going;            // ignores failures and writes on
    bool claim_success;         // returns 0 even after a failure
    unsigned runs;
} toy_t;

// Write i (from 1) fills toy_seq[i - 1] with i; each write reads its sector first.
static int toy_repair(const fault_io_t *io, void *ctx) {
    toy_t *t = ctx;
    uint8_t buf[SECTOR];
    bool failed = false;
    t->runs++;
    for (size_t i = 0; i < TOY_N; i++) {
        size_t k = i;
        if (t->nondeterministic && t->runs % 2 && (i == 1 || i == 2))
            k = 3 - i;                  // odd runs swap writes 2 and 3
        if (!io->read(io->ctx, toy_seq[k], 1, buf)) {
            failed = true;
            if (!t->keep_going)
                break;
        }
        fill(buf, (uint8_t)(k + 1));
        if (!io->write(io->ctx, toy_seq[k], 1, buf)) {
            failed = true;
            if (!t->keep_going)
                break;
        }
    }
    return failed && !t->claim_success ? 1 : 0;
}

static unsigned after_calls, clean_calls;

// Fails exactly when writes 1-3 have landed and write 4 hasn't.
static bool toy_after(fsck_disk_t *d, void *ctx, FILE *out) {
    (void)ctx;
    after_calls++;
    bool bad = first_byte(d, 102) == 3 && first_byte(d, 100) == 1;
    if (bad && out)
        fprintf(out, "  toy: the state after write 3\n");
    return !bad;
}

static bool toy_clean(fsck_disk_t *d, void *ctx, FILE *out) {
    (void)ctx;
    (void)out;
    clean_calls++;
    return first_byte(d, 100) == 4 && first_byte(d, 101) == 2 && first_byte(d, 102) == 3 &&
           first_byte(d, 103) == 5 && first_byte(d, 104) == 6;
}

static bool always(fsck_disk_t *d, void *ctx, FILE *out) {
    (void)d;
    (void)ctx;
    (void)out;
    return true;
}

TEST(runner_visits_every_n) {
    fsck_disk_t *d = fsck_disk_new(1000);
    toy_t t = {0};
    fault_run_t r = {.name = "toy", .seed = 1, .disk = d, .repair = toy_repair, .repair_ctx = &t,
                     .after = always, .clean = toy_clean};
    fault_result_t res;
    CHECK(fault_dry_run(&r, &res));
    CHECK_EQ(res.total_writes, TOY_N);
    CHECK_EQ(res.total_reads, TOY_N);
    CHECK_EQ(first_byte(d, 100), 0);                // the dry run is undone

    clean_calls = 0;
    CHECK_EQ(fault_run(&r, FAULT_CUT, &res), 0);
    CHECK_EQ(res.cases, TOY_N + 1);                 // N = 0 … 6
    CHECK_EQ(clean_calls, 1 + TOY_N + 1);           // the dry run, then once per case
    CHECK_EQ(fault_run(&r, FAULT_FAIL_WRITE, &res), 0);
    CHECK_EQ(res.cases, TOY_N);
    CHECK_EQ(fault_run(&r, FAULT_FAIL_READ, &res), 0);
    CHECK_EQ(res.cases, TOY_N);
    CHECK_EQ(fault_run_all(&r, &res), 0);
    CHECK_EQ(res.cases, 3 * TOY_N + 1);
    CHECK_EQ(first_byte(d, 100), 0);
    fsck_disk_free(d);
}

static char *capture_buf;
static size_t capture_len;

TEST(runner_catches_and_reports) {
    fsck_disk_t *d = fsck_disk_new(1000);
    toy_t t = {0};
    FILE *out = open_memstream(&capture_buf, &capture_len);
    fault_run_t r = {.name = "toy", .seed = 1234, .disk = d, .repair = toy_repair,
                     .repair_ctx = &t, .after = toy_after, .clean = toy_clean, .out = out};
    fault_result_t res;
    after_calls = 0;
    CHECK_EQ(fault_run(&r, FAULT_CUT, &res), 1);
    CHECK_EQ(res.first_failure, 3);
    CHECK_EQ(after_calls, TOY_N + 1);
    // A failed write 4 or read 4 leaves the same state.
    CHECK_EQ(fault_run(&r, FAULT_FAIL_WRITE, &res), 1);
    CHECK_EQ(res.first_failure, 4);
    CHECK_EQ(fault_run(&r, FAULT_FAIL_READ, &res), 1);
    CHECK_EQ(res.first_failure, 4);
    fclose(out);
    CHECK(strstr(capture_buf, "fault run toy, seed 1234: cut after N=3 (of 6)") != NULL);
    CHECK(strstr(capture_buf, "toy: the state after write 3") != NULL);
    CHECK(strstr(capture_buf, "    #3 lba 102\n    #4 lba 100 (dropped)") != NULL);
    CHECK(strstr(capture_buf, "fail write N=4 (of 6)") != NULL);
    CHECK(strstr(capture_buf, "fail read N=4 (of 6)") != NULL);
    free(capture_buf);

    // Repair that isn't deterministic, writes after a failure, or claims success.
    out = open_memstream(&capture_buf, &capture_len);
    r.out = out;
    r.after = always;
    t = (toy_t){.nondeterministic = true};
    CHECK(fault_run(&r, FAULT_CUT, &res) > 0);
    t = (toy_t){.keep_going = true};
    CHECK_EQ(fault_run(&r, FAULT_FAIL_WRITE, &res), TOY_N - 1);    // all but the last write
    t = (toy_t){.claim_success = true};
    CHECK_EQ(fault_run(&r, FAULT_FAIL_READ, &res), TOY_N);
    fclose(out);
    CHECK(strstr(capture_buf, "isn't deterministic") != NULL);
    CHECK(strstr(capture_buf, "repair wrote after a failed read or write") != NULL);
    CHECK(strstr(capture_buf, "(after the failure)") != NULL);
    CHECK(strstr(capture_buf, "repair reported success after a failed read or write") != NULL);
    free(capture_buf);
    fsck_disk_free(d);
}

// --- fingerprints ---------------------------------------------------------------------------

static uint64_t cluster_lba(const damage_fat32_t *g, uint32_t c) {
    return g->database + (uint64_t)(c - 2) * g->spc;
}

static void flip(fsck_disk_t *d, uint64_t lba, uint32_t ofs) {
    uint8_t s[SECTOR];
    fsck_disk_get(d, lba, s);
    s[ofs] ^= 0x40;
    fsck_disk_put(d, lba, s);
}

TEST(fingerprints) {
    damage_fat32_t g;
    CHECK(damage_fat32_parse(base1, &g));
    fsck_fp_t again, changed;
    CHECK(fsck_fp_take(base1, &again));
    CHECK_EQ(fsck_fp_diff(&base1_fp, &again, NULL, 0, NULL), 0);
    CHECK_EQ(fsck_fnv1a64(FSCK_FNV1A64_INIT, "a", 1), 0xAF63DC4C8601EC8Cull);   // the FNV vector

    // One byte of a file: that file differs, and nothing else.
    const char *path = "/dir/sub/deep.bin";
    int i = damage_catalog_find(&base1_cat, path);
    CHECK(i >= 0);
    if (i < 0)
        return;
    fsck_disk_snapshot(base1);
    flip(base1, cluster_lba(&g, base1_cat.e[i].clust) + 1, 17);
    CHECK(fsck_fp_take(base1, &changed));
    FILE *out = open_memstream(&capture_buf, &capture_len);
    CHECK_EQ(fsck_fp_diff(&base1_fp, &changed, NULL, 0, out), 1);
    fclose(out);
    CHECK(!strcmp(capture_buf, "  /dir/sub/deep.bin: bytes differ\n"));
    free(capture_buf);
    const char *ex[] = {path};
    CHECK_EQ(fsck_fp_diff(&base1_fp, &changed, ex, 1, NULL), 0);
    const char *ex_dir[] = {"/dir/"};
    CHECK_EQ(fsck_fp_diff(&base1_fp, &changed, ex_dir, 1, NULL), 0);
    const char *ex_other[] = {"/di"};
    CHECK_EQ(fsck_fp_diff(&base1_fp, &changed, ex_other, 1, NULL), 1);
    fsck_fp_free(&changed);
    fsck_disk_restore(base1);

    // Bytes no file holds: the slack after a file's end, a free cluster.
    i = damage_catalog_find(&base1_cat, "/README.TXT");        // 100 bytes in a 1 KB cluster
    CHECK(i >= 0);
    if (i < 0)
        return;
    flip(base1, cluster_lba(&g, base1_cat.e[i].clust), 200);
    flip(base1, cluster_lba(&g, g.count), 5);
    CHECK(fsck_fp_take(base1, &changed));
    CHECK_EQ(fsck_fp_diff(&base1_fp, &changed, NULL, 0, NULL), 0);
    fsck_fp_free(&changed);
    fsck_disk_restore(base1);

    // The write probe adds one file, and only that.
    CHECK(fsck_fp_write_probe(base1, "/probe.bin", 5000));
    CHECK(fsck_fp_take(base1, &changed));
    CHECK_EQ(fsck_fp_diff(&base1_fp, &changed, NULL, 0, NULL), 1);
    const char *ex_probe[] = {"/probe.bin"};
    CHECK_EQ(fsck_fp_diff(&base1_fp, &changed, ex_probe, 1, NULL), 0);
    fsck_fp_free(&changed);
    fsck_disk_restore(base1);
    fsck_fp_free(&again);
}

// --- damage, one kind at a time -------------------------------------------------------------

static void invalidate_fsinfo(fsck_disk_t *d, const damage_fat32_t *g) {
    uint8_t s[SECTOR];
    fsck_disk_get(d, g->vol + g->fsinfo, s);
    memset(s + 488, 0xFF, 4);
    fsck_disk_put(d, g->vol + g->fsinfo, s);
}

static uint32_t fatfs_free(fsck_disk_t *d) {
    DWORD n = 0;
    FATFS *fs;
    CHECK(fsck_disk_mount(d));
    CHECK_EQ(f_getfree("0:", &n, &fs), FR_OK);
    fsck_disk_unmount();
    return (uint32_t)n;
}

static uint64_t fsize_of(fsck_disk_t *d, const char *path) {
    FILINFO info;
    CHECK(fsck_disk_mount(d));
    CHECK_EQ(f_stat(path, &info), FR_OK);
    fsck_disk_unmount();
    return info.fsize;
}

// The sectors a fault may change: its clusters' FAT entries in every copy, FSInfo, and for a
// size, the file's directory sector.
static bool sector_claimed(const damage_fat32_t *g, const damage_fault_t *f,
                           const damage_catalog_t *cat, uint64_t lba) {
    if (lba == g->vol + g->fsinfo)
        return true;
    for (uint32_t k = 0; k < g->nfats; k++)
        for (uint32_t i = 0; i < f->n_clusters; i++)
            if (lba == g->fatbase + (uint64_t)k * g->fatsz + f->clusters[i] / 128)
                return true;
    return f->file[0] >= 0 && lba == cat->e[f->file[0]].dir_lba;
}

typedef struct {
    fsck_disk_t *d;
    const damage_catalog_t *cat;
    const fsck_fp_t *fp;
    damage_fat32_t g;
    uint32_t base_free;
} kind_case_t;

static void check_kind(kind_case_t *kc, unsigned kind, uint64_t seed) {
    fsck_disk_t *d = kc->d;
    const damage_fat32_t *g = &kc->g;
    damage_fault_t f;
    fsck_disk_restore(d);
    CHECK(damage_apply(d, kc->cat, &damage_fat32_kinds[kind], seed, &f));
    CHECK(f.kind == &damage_fat32_kinds[kind]);

    // Only the sectors it claims changed.
    static uint64_t changed[64];
    size_t n = fsck_disk_changed(d, changed, 64);
    CHECK(n > 0 && n <= 64);
    for (size_t i = 0; i < n && i < 64; i++)
        CHECK(sector_claimed(g, &f, kc->cat, changed[i]));

    // FatFs still mounts it; files read the same but for the ones it says it broke.
    fsck_fp_t fp;
    CHECK(fsck_fp_take(d, &fp));
    const char *broken[2];
    size_t nb = 0;
    for (unsigned s = 0; s < 2; s++)
        if (f.file[s] >= 0 && f.broken[s])
            broken[nb++] = kc->cat->e[f.file[s]].path;
    CHECK_EQ(fsck_fp_diff(kc->fp, &fp, broken, nb, stderr), 0);

    const damage_entry_t *a = f.file[0] >= 0 ? &kc->cat->e[f.file[0]] : NULL;
    const damage_entry_t *b = f.file[1] >= 0 ? &kc->cat->e[f.file[1]] : NULL;
    bool needs_a = kind != DAMAGE_LOST_CHAIN && kind != DAMAGE_STALE_FSINFO &&
                   kind != DAMAGE_FAT_COPIES;
    bool needs_b = kind == DAMAGE_CROSS_LINK || kind == DAMAGE_OVERLONG_XLINK;
    CHECK(!needs_a || a);
    CHECK(!needs_b || b);
    if ((needs_a && !a) || (needs_b && !b)) {
        fsck_fp_free(&fp);
        fsck_disk_restore(d);
        return;
    }
    static uint32_t chain[1024];
    bool ended = false;
    uint32_t len = a ? damage_chain(d, g, a->clust, chain, 1024, &ended) : 0;
    uint32_t counted = damage_count_free(d, g);
    if (kind != DAMAGE_STALE_FSINFO)
        CHECK_EQ(damage_fsinfo_free(d, g), counted);         // kept true
    switch (kind) {
    case DAMAGE_LOST_CHAIN: {
        uint32_t lost = damage_chain(d, g, f.clusters[0], NULL, 0, &ended);
        CHECK(ended);
        CHECK_EQ(lost, f.n_clusters);
        CHECK_EQ(counted, kc->base_free - f.n_clusters);
        invalidate_fsinfo(d, g);                             // so FatFs counts the FAT
        CHECK_EQ(fatfs_free(d), kc->base_free - f.n_clusters);
        CHECK_EQ(fsck_fp_diff(kc->fp, &fp, NULL, 0, NULL), 0);
        break;
    }
    case DAMAGE_OVERLONG:
        CHECK(!f.broken[0]);
        CHECK(ended);
        CHECK_EQ(len, f.value[0] + f.value[1]);
        CHECK_EQ(f.value[0], damage_needed(g, a->size));
        CHECK_EQ(fsize_of(d, a->path), a->size);
        CHECK_EQ(chain[f.value[0] - 1], f.clusters[0]);
        CHECK_EQ(counted, kc->base_free - f.value[1]);
        break;
    case DAMAGE_STALE_FSINFO:
        CHECK_EQ(f.value[0], counted);
        CHECK(f.value[1] != counted);
        CHECK_EQ(damage_fsinfo_free(d, g), f.value[1]);
        CHECK_EQ(fatfs_free(d), f.value[1]);                 // FatFs believes it
        invalidate_fsinfo(d, g);
        CHECK_EQ(fatfs_free(d), counted);
        break;
    case DAMAGE_FAT_COPIES:
        CHECK_EQ(n, 1);
        CHECK(changed[0] >= g->fatbase + g->fatsz);          // in copy 1
        CHECK_EQ(damage_fat_get(d, g, 0, f.clusters[0]), 0);
        CHECK_EQ(damage_fat_get(d, g, 1, f.clusters[0]), DAMAGE_EOC);
        CHECK_EQ(counted, kc->base_free);
        break;
    case DAMAGE_CROSS_LINK: {
        CHECK(f.broken[0] && !f.broken[1]);
        CHECK(ended);
        CHECK_EQ(len, f.value[0]);
        bool reaches = false;
        for (uint32_t i = 0; i < len && i < 1024; i++)
            reaches |= chain[i] == f.clusters[1];
        CHECK(reaches);
        CHECK(fsck_fp_diff(kc->fp, &fp, NULL, 0, NULL) >= 1);   // A reads differently
        for (uint32_t i = 2; i < f.n_clusters; i++)          // A's old tail is free
            CHECK_EQ(damage_fat_get(d, g, 0, f.clusters[i]), 0);
        break;
    }
    case DAMAGE_BROKEN_CHAIN:
        CHECK(f.broken[0]);
        CHECK(!ended);
        CHECK_EQ(damage_fat_get(d, g, 0, f.clusters[0]), 0);
        CHECK_EQ(counted, kc->base_free + 1);
        CHECK_EQ(len + f.value[0], damage_needed(g, a->size));  // up to the free one, then lost
        break;
    case DAMAGE_SIZE_LARGER: {
        CHECK(f.broken[0]);
        CHECK(ended);
        CHECK_EQ(fsize_of(d, a->path), f.value[1]);
        CHECK(f.value[1] > (uint64_t)len * g->spc * SECTOR);
        int k = -1;
        for (size_t i = 0; i < fp.n; i++)
            if (!strcmp(fp.e[i].path, a->path))
                k = (int)i;
        CHECK(k >= 0 && !fp.e[k].read_ok);                   // FatFs runs off the chain
        break;
    }
    case DAMAGE_OVERLONG_XLINK: {
        CHECK(!f.broken[0] && !f.broken[1]);
        CHECK(f.file[0] < f.file[1]);                        // B is later in the walk
        CHECK(ended);
        CHECK_EQ(len, f.value[0] + f.value[1]);
        CHECK(len > damage_needed(g, a->size));
        uint32_t lb = damage_chain(d, g, b->clust, NULL, 0, &ended);
        CHECK(ended);
        CHECK_EQ(damage_needed(g, b->size), lb);
        CHECK_EQ(fsck_fp_diff(kc->fp, &fp, NULL, 0, NULL), 0);  // both read the same
        CHECK_EQ(counted, kc->base_free);
        break;
    }
    default:
        CHECK(false);
    }
    fsck_fp_free(&fp);
    fsck_disk_restore(d);
}

TEST(damage_kinds) {
    for (int two = 0; two < 2; two++) {
        kind_case_t kc = {.d = two ? base2 : base1, .cat = two ? &base2_cat : &base1_cat,
                          .fp = two ? &base2_fp : &base1_fp};
        CHECK(damage_fat32_parse(kc.d, &kc.g));
        kc.base_free = damage_count_free(kc.d, &kc.g);
        fsck_disk_snapshot(kc.d);
        for (unsigned kind = 0; kind < DAMAGE_FAT32_KINDS; kind++) {
            if (kind == DAMAGE_FAT_COPIES && !two) {         // needs two FATs
                damage_fault_t f;
                CHECK(!damage_apply(kc.d, kc.cat, &damage_fat32_kinds[kind], 1, &f));
                uint64_t lba;
                CHECK_EQ(fsck_disk_changed(kc.d, &lba, 1), 0);
                continue;
            }
            for (uint64_t seed = 1; seed <= 6; seed++)
                check_kind(&kc, kind, seed * 7919 + kind);
        }
        fsck_disk_restore(kc.d);
    }
}

// --- random damage --------------------------------------------------------------------------

#define SEEDS 300

static bool same_fault(const damage_fault_t *x, const damage_fault_t *y) {
    return x->kind == y->kind && x->file[0] == y->file[0] && x->file[1] == y->file[1] &&
           x->n_clusters == y->n_clusters && x->value[0] == y->value[0] &&
           x->value[1] == y->value[1] &&
           !memcmp(x->clusters, y->clusters, sizeof(x->clusters)) && !strcmp(x->what, y->what);
}

TEST(damage_random_seeds) {
    unsigned seen[DAMAGE_FAT32_KINDS] = {0}, faults[DAMAGE_FAULTS_MAX + 1] = {0};
    fsck_disk_snapshot(base1);
    fsck_disk_snapshot(base2);
    for (uint64_t seed = 1; seed <= SEEDS; seed++) {
        bool two = seed % 2;
        fsck_disk_t *d = two ? base2 : base1;
        const damage_catalog_t *cat = two ? &base2_cat : &base1_cat;
        static damage_t dm, again;
        fsck_disk_restore(d);
        CHECK(damage_random(d, cat, damage_fat32_kinds, DAMAGE_FAT32_KINDS, seed, &dm));
        CHECK(dm.n >= 1 && dm.n <= DAMAGE_FAULTS_MAX);
        CHECK(dm.f[0].kind->repairable);
        if (dm.n <= DAMAGE_FAULTS_MAX)
            faults[dm.n]++;
        for (unsigned i = 0; i < dm.n; i++)
            seen[dm.f[i].kind - damage_fat32_kinds]++;
        fsck_disk_t *first = fsck_disk_copy(d);

        // The same seed, the same damage.
        fsck_disk_restore(d);
        CHECK(damage_random(d, cat, damage_fat32_kinds, DAMAGE_FAT32_KINDS, seed, &again));
        CHECK_EQ(again.n, dm.n);
        for (unsigned i = 0; i < dm.n && i < again.n; i++)
            CHECK(same_fault(&dm.f[i], &again.f[i]));
        CHECK(fsck_disk_equal(first, d));
        fsck_disk_free(first);

        // FatFs mounts it, and the files it didn't break read the same.
        fsck_fp_t fp;
        const char *broken[2 * DAMAGE_FAULTS_MAX];
        size_t nb = damage_broken_paths(&dm, cat, broken, 2 * DAMAGE_FAULTS_MAX);
        CHECK(fsck_fp_take(d, &fp));
        size_t diffs = fsck_fp_diff(two ? &base2_fp : &base1_fp, &fp, broken, nb, stderr);
        CHECK_EQ(diffs, 0);
        if (diffs)
            damage_print(&dm, stderr);
        fsck_fp_free(&fp);
    }
    fsck_disk_restore(base1);
    fsck_disk_restore(base2);
    for (unsigned k = 0; k < DAMAGE_FAT32_KINDS; k++)
        CHECK(seen[k] > 0);
    for (unsigned n = 1; n <= DAMAGE_FAULTS_MAX; n++)
        CHECK(faults[n] > 0);
    // Another seed, other damage.
    static damage_t x, y;
    CHECK(damage_random(base1, &base1_cat, damage_fat32_kinds, DAMAGE_FAT32_KINDS, 1, &x));
    fsck_disk_restore(base1);
    CHECK(damage_random(base1, &base1_cat, damage_fat32_kinds, DAMAGE_FAT32_KINDS, 3, &y));
    fsck_disk_restore(base1);
    CHECK(x.n != y.n || !same_fault(&x.f[0], &y.f[0]));
}

// --- the whole loop, with a toy repair over random damage -----------------------------------

// A stand-in repair that knows the damage: FSInfo invalidated first, then each lost chain
// freed, FAT copy 0 before copy 1, one sector per write, stopping at the first error (§10.2).
// `zero_first` makes it wrong: it first writes the FAT entry of a file's first cluster as free,
// and puts it back at the end.
typedef struct {
    const damage_t *dm;
    damage_fat32_t g;
    const damage_entry_t *victim;
    bool zero_first;
} lost_repair_t;

static bool set_entry(const fault_io_t *io, uint64_t lba, uint32_t ofs, uint32_t v) {
    uint8_t s[SECTOR];
    if (!io->read(io->ctx, lba, 1, s))
        return false;
    for (unsigned i = 0; i < 4; i++)
        s[ofs + i] = (uint8_t)(v >> (8 * i));
    return io->write(io->ctx, lba, 1, s);
}

// Set FAT entry `c` in every copy, copy 0 first. With `skip_same`, a copy that already holds
// `v` isn't written (an earlier run, cut short, may have done one copy and not the other).
static bool set_fat(const fault_io_t *io, const damage_fat32_t *g, uint32_t c, uint32_t v,
                    bool skip_same) {
    for (uint32_t k = 0; k < g->nfats; k++) {
        uint64_t lba = g->fatbase + (uint64_t)k * g->fatsz + c / 128;
        if (skip_same) {
            uint8_t s[SECTOR];
            if (!io->read(io->ctx, lba, 1, s))
                return false;
            const uint8_t *e = s + 4 * (c % 128);
            uint32_t old = (uint32_t)e[0] | (uint32_t)e[1] << 8 | (uint32_t)e[2] << 16 |
                           (uint32_t)(e[3] & 0x0F) << 24;
            if (old == v)
                continue;
        }
        if (!set_entry(io, lba, 4 * (c % 128), v))
            return false;
    }
    return true;
}

static uint32_t fat_read(const fault_io_t *io, const damage_fat32_t *g, uint32_t c, bool *ok) {
    uint8_t s[SECTOR];
    *ok = io->read(io->ctx, g->fatbase + c / 128, 1, s);
    const uint8_t *e = s + 4 * (c % 128);
    return *ok ? (uint32_t)e[0] | (uint32_t)e[1] << 8 | (uint32_t)e[2] << 16 |
                 (uint32_t)(e[3] & 0x0F) << 24 : 0;
}

static bool is_lost(const damage_fault_t *f) {
    return f->kind->apply == damage_fat32_kinds[DAMAGE_LOST_CHAIN].apply;
}

static int lost_repair(const fault_io_t *io, void *ctx) {
    const lost_repair_t *t = ctx;
    const damage_fat32_t *g = &t->g;
    uint32_t keep = 0;
    bool ok = true;
    if (t->zero_first) {
        keep = fat_read(io, g, t->victim->clust, &ok);
        if (!ok || !set_fat(io, g, t->victim->clust, 0, false))
            return 1;
    }
    if (!set_entry(io, g->vol + g->fsinfo, 488, UINT32_MAX))
        return 1;
    for (unsigned i = 0; i < t->dm->n; i++) {
        const damage_fault_t *f = &t->dm->f[i];
        if (!is_lost(f))
            continue;
        for (uint32_t k = 0; k < f->n_clusters; k++)
            if (!set_fat(io, g, f->clusters[k], 0, true))
                return 1;
    }
    if (t->zero_first && !set_fat(io, g, t->victim->clust, keep, false))
        return 1;
    return 0;
}

typedef struct {
    const fsck_fp_t *before;            // before the damage
    const char *broken[2 * DAMAGE_FAULTS_MAX];
    size_t n_broken;
    const lost_repair_t *repair;
} lost_props_t;

static bool files_same(fsck_disk_t *d, void *ctx, FILE *out) {
    const lost_props_t *p = ctx;
    fsck_fp_t fp;
    if (!fsck_fp_take(d, &fp)) {
        if (out)
            fprintf(out, "  host FatFs can't mount the image\n");
        return false;
    }
    size_t diffs = fsck_fp_diff(p->before, &fp, p->broken, p->n_broken, out);
    fsck_fp_free(&fp);
    return diffs == 0;
}

static bool lost_freed(fsck_disk_t *d, void *ctx, FILE *out) {
    const lost_props_t *p = ctx;
    const damage_fat32_t *g = &p->repair->g;
    for (unsigned i = 0; i < p->repair->dm->n; i++) {
        const damage_fault_t *f = &p->repair->dm->f[i];
        if (!is_lost(f))
            continue;
        for (uint32_t k = 0; k < f->n_clusters; k++)
            for (uint32_t c = 0; c < g->nfats; c++)
                if (damage_fat_get(d, g, c, f->clusters[k]) != 0) {
                    if (out)
                        fprintf(out, "  cluster %u still used in copy %u\n",
                                (unsigned)f->clusters[k], (unsigned)c);
                    return false;
                }
    }
    return damage_fsinfo_free(d, g) == UINT32_MAX && files_same(d, ctx, out) &&
           fsck_fp_write_probe(d, "/after repair.bin", 3000);
}

TEST(runner_over_random_damage) {
    // A table of a few kinds: its entries are copies, so a kind is known by its apply().
    const damage_kind_t table[] = {
        damage_fat32_kinds[DAMAGE_LOST_CHAIN],
        damage_fat32_kinds[DAMAGE_CROSS_LINK],
        damage_fat32_kinds[DAMAGE_SIZE_LARGER],
    };
    uint32_t cases = 0;
    for (uint64_t seed = 1; seed <= 8; seed++) {
        bool two = seed % 2;
        fsck_disk_t *d = fsck_disk_copy(two ? base2 : base1);
        const damage_catalog_t *cat = two ? &base2_cat : &base1_cat;
        static damage_t dm;
        CHECK(damage_random(d, cat, table, 3, seed, &dm));
        lost_repair_t rep = {.dm = &dm};
        CHECK(damage_fat32_parse(d, &rep.g));
        lost_props_t props = {.before = two ? &base2_fp : &base1_fp, .repair = &rep};
        props.n_broken = damage_broken_paths(&dm, cat, props.broken, 2 * DAMAGE_FAULTS_MAX);
        fault_run_t r = {.name = "lost chains", .seed = seed, .disk = d, .repair = lost_repair,
                         .repair_ctx = &rep, .after = files_same, .clean = lost_freed,
                         .prop_ctx = &props};
        fault_result_t res;
        CHECK_EQ(fault_run_all(&r, &res), 0);
        cases += res.cases;
        if (res.failures)
            damage_print(&dm, stderr);
        fsck_disk_free(d);
    }
    CHECK(cases > 30);

    // The wrong order is caught: cut after write 1, a file's first cluster reads as free.
    fsck_disk_t *d = fsck_disk_copy(base1);
    static const damage_t none = {.n = 0};
    lost_repair_t rep = {.dm = &none, .zero_first = true,
                         .victim = &base1_cat.e[damage_catalog_find(&base1_cat, "/notes.txt")]};
    CHECK(damage_fat32_parse(d, &rep.g));
    lost_props_t props = {.before = &base1_fp, .repair = &rep};
    FILE *out = open_memstream(&capture_buf, &capture_len);
    fault_run_t r = {.name = "wrong order", .disk = d, .repair = lost_repair,
                     .repair_ctx = &rep, .after = files_same, .clean = lost_freed,
                     .prop_ctx = &props, .out = out};
    fault_result_t res;
    CHECK(fault_run(&r, FAULT_CUT, &res) > 0);
    CHECK_EQ(res.first_failure, 1);
    fclose(out);
    CHECK(strstr(capture_buf, "/notes.txt") != NULL);
    free(capture_buf);
    fsck_disk_free(d);
}

// The base images, their fingerprints and catalogues, shared by the tests below. A test that
// changes a base image takes a snapshot and restores it, or works on a copy.
static void setup(void) {
    base1 = make_base(1);
    base2 = make_base(2);
    CHECK(fsck_fp_take(base1, &base1_fp));
    CHECK(fsck_fp_take(base2, &base2_fp));
    CHECK(damage_catalog(base1, &base1_cat));
    CHECK(damage_catalog(base2, &base2_cat));
}

static void teardown(void) {
    fsck_fp_free(&base1_fp);
    fsck_fp_free(&base2_fp);
    damage_catalog_free(&base1_cat);
    damage_catalog_free(&base2_cat);
    fsck_disk_free(base1);
    fsck_disk_free(base2);
}

TEST_MAIN(
    RUN(fault_layer_counts);
    RUN(fault_layer_cut);
    RUN(fault_layer_fail_write);
    RUN(fault_layer_fail_read);
    setup();
    RUN(builder);
    RUN(snapshot_restore_exact);
    RUN(runner_visits_every_n);
    RUN(runner_catches_and_reports);
    RUN(fingerprints);
    RUN(damage_kinds);
    RUN(damage_random_seeds);
    RUN(runner_over_random_damage);
    teardown();
)
