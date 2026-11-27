// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// src/files/fsck_repair.c: `sd check fix`'s FAT32 repairs (docs/design/sd-check.md §10), on
// images host FatFs made, damaged by tests/fsck_damage.c, with the faults of tests/fsck_faults.c
// (§10.4). The properties, for every case:
// - the plan (the first check) counts what the damage records say;
// - after repair, no problem of a repaired kind is left (unless the plan said it is skipped),
//   no new problem appears, every file reads the same bytes as before the repair, files the
//   damage didn't touch read as before the damage, and host FatFs writes a new file;
// - stopped after any write (cut, failed write, failed read), the volume is no worse: every
//   file reads the same, and a check reports a subset of the first check's problems, plus "the
//   two FATs differ" and lost clusters that are tails of chains being trimmed (§10.2); repair run
//   again then finishes clean;
// - a repair with a tiny window and a one-sector budget ends in the same image as one with the
//   board's 16 KB window;
// - no step reads more than its budget or transfers more than budget + 2 sectors, and every
//   write is one sector.

#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>

#include "files/fsck_repair.h"
#include "fsck_damage.h"
#include "fsck_disk.h"
#include "fsck_faults.h"
#include "test.h"

#define SECTOR        512u
#define WINDOW_BIG    16384u        // the board's window: the test images fit in one pass
#define WINDOW_TINY   64u           // 512 clusters a pass: 133 passes on the small images
#define LINES_MAX     64u
#define TAILS_MAX     64u
#define SEEDS         300u          // random damage, repaired and checked
#define FAULT_SEEDS   8u            // random damage, also through fault_run_all()
#define READ_STRIDE   500u          // fail read: every 500th read, and those around each write

// --- running a repair -----------------------------------------------------------------------

static uint8_t mem[32768];
static uint32_t step_violations;    // a step past its budget, or a write of several sectors

// A check's result and report lines.
typedef struct {
    fsck_disk_t *disk;
    int status;
    fsck_result_t res;
    unsigned n;
    char lines[LINES_MAX][FSCK_LINE_MAX];
} chk_t;

static void chk_add(chk_t *c, const char *line) {
    if (c->n < LINES_MAX)
        snprintf(c->lines[c->n++], FSCK_LINE_MAX, "%s", line);
}

// The last repair's first and final checks, as they reported.
static chk_t rep_check, rep_final;

typedef struct {
    const fsck_io_t *in;
    uint32_t reads, writes;
    chk_t *lines;                   // where the running check's lines go
} wrap_t;

static void wrap_report(void *ctx, const char *line) {
    const wrap_t *w = ctx;
    if (w->lines)
        chk_add(w->lines, line);
}

static bool wrap_read(void *ctx, uint64_t lba, uint32_t count, uint8_t *buf) {
    wrap_t *w = ctx;
    w->reads += count;
    return w->in->read(w->in->ctx, lba, count, buf);
}

static bool wrap_write(void *ctx, uint64_t lba, uint32_t count, const uint8_t *buf) {
    wrap_t *w = ctx;
    w->writes += count;
    if (count != 1)
        step_violations++;
    return w->in->write(w->in->ctx, lba, count, buf);
}

typedef struct {
    int status;                     // FSCK_DONE or FSCK_FAILED
    fsck_phase_t failed_in;         // the phase it ended in
    fsck_plan_t plan, done, left;
    fsck_result_t check, final;
    uint32_t writes;
} rep_t;

// Repair the device `io` describes, confirming the plan. 0 when it finished.
static int repair_with(const fsck_io_t *io, size_t window, uint32_t budget, rep_t *out) {
    wrap_t w = {.in = io};
    fsck_io_t wio = {.read = wrap_read, .report = wrap_report, .write = wrap_write, .ctx = &w,
                     .sectors = io->sectors};
    memset(out, 0, sizeof(*out));
    rep_check.n = rep_final.n = 0;
    rep_check.status = rep_final.status = FSCK_FAILED;
    fsck_repair_t *r = fsck_repair_new(&wio, mem, fsck_repair_state_size() + window);
    if (!r)
        abort();
    for (;;) {
        w.reads = w.writes = 0;
        fsck_phase_t phase = fsck_repair_phase(r);
        w.lines = phase == FSCK_PHASE_CHECK ? &rep_check : phase == FSCK_PHASE_FINAL ? &rep_final
                                                                                  : NULL;
        int st = fsck_repair_step(r, budget);
        uint32_t b = budget ? budget : 1;
        if (w.reads > b || w.reads + w.writes > b + 2)
            step_violations++;
        if (st == FSCK_PLAN) {
            out->plan = *fsck_repair_plan(r);
            out->check = *fsck_repair_check(r);
            rep_check.res = out->check;
            rep_check.status = FSCK_DONE;
            fsck_repair_confirm(r);
            if (fsck_repair_phase(r) == FSCK_PHASE_DONE)
                rep_final = rep_check;          // nothing to write: the final check is the first
        } else if (st == FSCK_DONE || st == FSCK_FAILED) {
            out->status = st;
            out->failed_in = phase;
            out->done = *fsck_repair_done(r);
            out->left = *fsck_repair_left(r);
            out->final = *fsck_repair_result(r);
            out->writes = fsck_repair_writes(r);
            if (st == FSCK_DONE && phase == FSCK_PHASE_FINAL) {
                rep_final.res = out->final;
                rep_final.status = FSCK_DONE;
            }
            if (fsck_repair_step(r, budget) != st)    // it stays where it ended
                step_violations++;
            return st == FSCK_DONE ? 0 : 1;
        }
    }
}

// --- a plain check, with its lines ----------------------------------------------------------

static bool chk_read(void *ctx, uint64_t lba, uint32_t count, uint8_t *buf) {
    const chk_t *c = ctx;
    return fsck_disk_read(c->disk, lba, count, buf);
}

static void chk_report(void *ctx, const char *line) {
    chk_add(ctx, line);
}

static void check_disk(fsck_disk_t *d, chk_t *c) {
    static uint8_t cmem[32768];
    c->disk = d;
    c->n = 0;
    fsck_io_t io = {.read = chk_read, .report = chk_report, .ctx = c,
                    .sectors = fsck_disk_sectors(d)};
    fsck_t *f = fsck_new(&io, cmem, fsck_state_size() + WINDOW_BIG);
    if (!f)
        abort();
    do
        c->status = fsck_step(f, 256);
    while (c->status == FSCK_MORE);
    c->res = *fsck_result(f);
}

static bool starts(const char *s, const char *prefix) {
    return !strncmp(s, prefix, strlen(prefix));
}

static bool is_note(const char *l) {
    return starts(l, "note: ");
}

static bool is_lost(const char *l) {
    return starts(l, "lost: ");
}

static bool is_fat_diff(const char *l) {
    return starts(l, "the two FATs differ");
}

static bool is_stale_fsinfo(const char *l) {
    return starts(l, "note: the FSInfo free count");
}

// "note: <path>: N clusters for M bytes, chain has K" with N >= 1: a chain repair trims.
static bool is_trimmable(const char *l) {
    const char *p = strrchr(l, ':');
    unsigned need, n;
    uint32_t size;
    return is_note(l) && p && sscanf(p + 1, " %u %*s for %" SCNu32 " bytes, chain has %u", &need,
                                     &size, &n) == 3 && n > need && need >= 1;
}

// "lost: cluster A (…)" or "lost: clusters A-B (…)".
static bool lost_range(const char *l, uint32_t *a, uint32_t *b) {
    if (sscanf(l, "lost: clusters %" SCNu32 "-%" SCNu32, a, b) == 2)
        return true;
    if (sscanf(l, "lost: cluster %" SCNu32, a) == 1) {
        *b = *a;
        return true;
    }
    return false;
}

static bool has_line(const chk_t *c, const char *line) {
    for (unsigned i = 0; i < c->n; i++)
        if (!strcmp(c->lines[i], line))
            return true;
    return false;
}

// --- a case: a damaged image and what is known about it -------------------------------------

typedef struct {
    const char *name;
    uint64_t seed;
    fsck_disk_t *d;                 // damaged; the runner keeps its snapshot
    damage_t dm;
    damage_fat32_t g;
    const fsck_fp_t *base_fp;       // before the damage
    fsck_fp_t dmg_fp;               // damaged, before repair
    chk_t before;                   // the check of the damaged image
    const char *broken[2 * DAMAGE_FAULTS_MAX];
    size_t n_broken;
    uint32_t tails[TAILS_MAX];      // clusters a trim leaves lost until they are freed
    size_t n_tails;
    fsck_plan_t want;               // the plan the damage records predict
    size_t window;
    uint32_t budget;
    rep_t rep;                      // the last repair's
    bool clean_known;
    uint64_t clean_hash;            // an image already found clean
} case_t;

static bool in_tails(const case_t *c, uint32_t cl) {
    for (size_t i = 0; i < c->n_tails; i++)
        if (c->tails[i] == cl)
            return true;
    return false;
}

static bool lost_before(const case_t *c, uint32_t cl) {
    for (unsigned i = 0; i < c->before.n; i++) {
        uint32_t a, b;
        if (is_lost(c->before.lines[i]) && lost_range(c->before.lines[i], &a, &b) && cl >= a &&
            cl <= b)
            return true;
    }
    return false;
}

// The plan the damage predicts (fsck_damage.h says what each kind records).
static void predict(case_t *c) {
    fsck_plan_t *p = &c->want;
    memset(p, 0, sizeof(*p));
    c->n_tails = 0;
    uint64_t fat_secs[DAMAGE_FAULTS_MAX];
    unsigned n_secs = 0;
    bool stale = false;
    for (unsigned i = 0; i < c->dm.n; i++) {
        const damage_fault_t *f = &c->dm.f[i];
        unsigned kind = (unsigned)(f->kind - damage_fat32_kinds);
        switch (kind) {
        case DAMAGE_LOST_CHAIN:
            p->lost_clusters += f->n_clusters;
            break;
        case DAMAGE_BROKEN_CHAIN:
            p->lost_clusters += f->value[0];
            break;
        case DAMAGE_OVERLONG:
            p->trims++;
            p->trim_clusters += f->value[1];
            for (uint32_t k = 1; k < f->n_clusters && c->n_tails < TAILS_MAX; k++)
                c->tails[c->n_tails++] = f->clusters[k];
            break;
        case DAMAGE_OVERLONG_XLINK:
            p->trims++;
            p->trim_clusters += f->value[1];
            p->cross_links = true;
            break;
        case DAMAGE_CROSS_LINK:
            p->cross_links = true;
            if (f->value[0] > f->value[1]) {
                p->trims++;
                p->trim_clusters += f->value[0] - f->value[1];
            }
            break;
        case DAMAGE_STALE_FSINFO:
            stale = true;
            break;
        case DAMAGE_FAT_COPIES: {
            uint64_t s = f->clusters[0] / 128;
            bool seen = false;
            for (unsigned k = 0; k < n_secs; k++)
                seen |= fat_secs[k] == s;
            if (!seen)
                fat_secs[n_secs++] = s;
            break;
        }
        default:                        // DAMAGE_SIZE_LARGER: nothing to repair
            break;
        }
    }
    p->fat_sectors = n_secs;
    p->fsinfo = stale || fsck_plan_writes(p);
}

static void case_free(case_t *c) {
    fsck_fp_free(&c->dmg_fp);
}

// Set up a case on the damaged image `c->d` (damage in c->dm).
static void case_setup(case_t *c, const damage_catalog_t *cat, const fsck_fp_t *base_fp) {
    c->base_fp = base_fp;
    CHECK(damage_fat32_parse(c->d, &c->g));
    CHECK(fsck_fp_take(c->d, &c->dmg_fp));
    c->n_broken = damage_broken_paths(&c->dm, cat, c->broken, 2 * DAMAGE_FAULTS_MAX);
    predict(c);
    c->clean_known = false;
}

// --- the properties -------------------------------------------------------------------------

__attribute__((format(printf, 3, 4)))
static void say(FILE *out, const case_t *c, const char *fmt, ...) {
    if (!out)
        return;
    fprintf(out, "  %s, seed %" PRIu64 ": ", c->name, c->seed);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
}

// The image's sectors changed since the snapshot, hashed in LBA order.
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static uint64_t image_hash(const fsck_disk_t *d) {
    static uint64_t lbas[4096];
    static uint8_t s[SECTOR];
    size_t n = fsck_disk_changed(d, lbas, 4096);
    if (n > 4096)
        return 0;
    qsort(lbas, n, sizeof(lbas[0]), cmp_u64);
    uint64_t h = fsck_fnv1a64(FSCK_FNV1A64_INIT, &n, sizeof(n));
    for (size_t i = 0; i < n; i++) {
        fsck_disk_get(d, lbas[i], s);
        h = fsck_fnv1a64(h, &lbas[i], sizeof(lbas[i]));
        h = fsck_fnv1a64(h, s, SECTOR);
    }
    return h;
}

// Every file reads as in `fp` (but the excluded).
static bool files_as(fsck_disk_t *d, const fsck_fp_t *fp, const char *const *ex, size_t n_ex,
                     FILE *out) {
    fsck_fp_t now;
    if (!fsck_fp_take(d, &now)) {
        if (out)
            fprintf(out, "  host FatFs can't mount the image or read a directory\n");
        fsck_fp_free(&now);
        return false;
    }
    size_t diffs = fsck_fp_diff(fp, &now, ex, n_ex, out);
    fsck_fp_free(&now);
    return diffs == 0;
}

// The problems `now` reports are the first check's, but for FAT copies differing and lost
// clusters that were lost before or are tails of chains being trimmed (allow_tails).
static bool subset(const case_t *c, const chk_t *now, bool allow_tails, FILE *out) {
    if (now->status != FSCK_DONE || now->res.reported != now->res.problems + now->res.notes) {
        say(out, c, "%s", "the check failed or its lines were cut short");
        return false;
    }
    for (unsigned i = 0; i < now->n; i++) {
        const char *l = now->lines[i];
        if (is_note(l) || is_fat_diff(l))
            continue;
        uint32_t a, b;
        if (is_lost(l) && lost_range(l, &a, &b)) {
            for (uint32_t cl = a; cl <= b; cl++)
                if (!lost_before(c, cl) && !(allow_tails && in_tails(c, cl))) {
                    say(out, c, "newly lost: %s", l);
                    return false;
                }
            continue;
        }
        if (!has_line(&c->before, l)) {
            say(out, c, "a new problem: %s", l);
            return false;
        }
    }
    if (now->res.cross_linked > c->before.res.cross_linked) {
        say(out, c, "%s", "more cross-linked clusters");
        return false;
    }
    return true;
}

// §10.2: after a repair stopped part way.
static bool no_worse(fsck_disk_t *d, void *ctx, FILE *out) {
    case_t *c = ctx;
    uint64_t lba;
    if (fsck_disk_changed(d, &lba, 1) == 0)
        return true;                    // nothing written: the image as it was
    if (c->rep.status == FSCK_FAILED && c->rep.failed_in == FSCK_PHASE_CHECK) {
        say(out, c, "%s", "the first check failed, yet something was written");
        return false;
    }
    // §10.2: FSInfo first. Once a FAT sector has changed, FSInfo's free count is unknown (or
    // still true).
    static uint64_t lbas[4096];
    size_t n = fsck_disk_changed(d, lbas, 4096);
    uint64_t fat_end = c->g.fatbase + (uint64_t)c->g.nfats * c->g.fatsz;
    bool fat_changed = false;
    for (size_t i = 0; i < n && i < 4096; i++)
        fat_changed |= lbas[i] >= c->g.fatbase && lbas[i] < fat_end;
    uint32_t fsi = fat_changed ? damage_fsinfo_free(d, &c->g) : UINT32_MAX;
    if (fsi != UINT32_MAX && fsi != damage_count_free(d, &c->g)) {
        say(out, c, "%s", "the FAT changed before FSInfo was invalidated");
        return false;
    }
    // A repair that finished (a cut) checked what it left; else check it here.
    static chk_t now;
    const chk_t *seen = &rep_final;
    if (c->rep.status != FSCK_DONE) {
        check_disk(d, &now);
        seen = &now;
    }
    bool trims = !c->want.cross_links;
    return subset(c, seen, trims, out) && files_as(d, &c->dmg_fp, NULL, 0, out);
}

// After a repair that finished.
static bool clean(fsck_disk_t *d, void *ctx, FILE *out) {
    case_t *c = ctx;
    uint64_t h = image_hash(d);
    if (c->clean_known && h == c->clean_hash)
        return true;                    // the same image as one found clean
    const rep_t *r = &c->rep;
    if (r->status != FSCK_DONE) {
        say(out, c, "%s", "repair didn't finish");
        return false;
    }
    if (fsck_plan_writes(&r->left)) {
        say(out, c, "%s", "the final check still has something to repair");
        return false;
    }
    const chk_t *now = &rep_final;          // the final check of the repair just run
    if (!subset(c, now, false, out))
        return false;
    bool trims = !c->want.cross_links;
    for (unsigned i = 0; i < now->n; i++) {
        const char *l = now->lines[i];
        if (is_lost(l) || is_fat_diff(l) || is_stale_fsinfo(l) || (trims && is_trimmable(l))) {
            say(out, c, "left after repair: %s", l);
            return false;
        }
    }
    if (now->res.lost_clusters ||
        now->res.long_chains != (trims ? 0 : c->before.res.long_chains)) {
        say(out, c, "%s", "lost clusters or long chains left after repair");
        return false;
    }
    uint32_t fsi = damage_fsinfo_free(d, &c->g);
    if (fsi != UINT32_MAX && fsi != damage_count_free(d, &c->g)) {
        say(out, c, "%s", "the FSInfo free count is stale");
        return false;
    }
    if (!files_as(d, &c->dmg_fp, NULL, 0, out) ||
        !files_as(d, c->base_fp, c->broken, c->n_broken, out))
        return false;
    if (!fsck_fp_write_probe(d, "/after repair.bin", 5000)) {
        say(out, c, "%s", "host FatFs can't write and read back a new file");
        return false;
    }
    c->clean_known = true;
    c->clean_hash = h;
    return true;
}

static int run_repair(const fsck_io_t *io, void *ctx) {
    case_t *c = ctx;
    return repair_with(io, c->window, c->budget, &c->rep);
}

static bool plan_is(const case_t *c, const fsck_plan_t *p) {
    const fsck_plan_t *w = &c->want;
    return p->lost_clusters == w->lost_clusters && p->trims == w->trims &&
           p->trim_clusters == w->trim_clusters && p->fat_sectors == w->fat_sectors &&
           p->fsinfo == w->fsinfo && p->cross_links == w->cross_links && !p->unread &&
           !p->exfat && !p->empty_chains;
}

// Repair once to the end and hold it to the properties; then, with `faults`, every fault.
static uint32_t fault_cases, fault_runs, cut_points;

static void repair_case(case_t *c, bool faults) {
    fsck_disk_snapshot(c->d);
    c->window = WINDOW_BIG;
    c->budget = 64;
    fsck_io_t io = {.read = fsck_disk_read, .write = fsck_disk_write, .ctx = c->d,
                    .sectors = fsck_disk_sectors(c->d)};
    CHECK_EQ(repair_with(&io, c->window, c->budget, &c->rep), 0);
    c->before = rep_check;
    CHECK(c->before.res.reported == c->before.res.problems + c->before.res.notes);
    bool plan_ok = plan_is(c, &c->rep.plan);
    CHECK(plan_ok);
    const fsck_plan_t *p = &c->rep.plan, *dn = &c->rep.done;
    bool trims = !p->cross_links;
    CHECK_EQ(dn->trims, trims ? p->trims : 0);
    CHECK_EQ(dn->lost_clusters, p->lost_clusters + (trims ? p->trim_clusters : 0));
    CHECK_EQ(dn->fat_sectors, p->fat_sectors);
    CHECK_EQ(dn->fsinfo, p->fsinfo);
    CHECK_EQ(c->rep.writes > 0, fsck_plan_writes(p));
    bool ok = clean(c->d, c, stderr);
    CHECK(ok);
    if (!plan_ok || !ok)
        damage_print(&c->dm, stderr);
    fsck_disk_restore(c->d);
    if (!faults)
        return;

    // The same end with a tiny window and a one-sector budget.
    fsck_disk_t *tiny = fsck_disk_copy(c->d);
    fsck_disk_t *big = fsck_disk_copy(c->d);
    rep_t rt, rb;
    fsck_io_t ti = {.read = fsck_disk_read, .write = fsck_disk_write, .ctx = tiny,
                    .sectors = fsck_disk_sectors(tiny)};
    fsck_io_t bi = ti;
    bi.ctx = big;
    CHECK_EQ(repair_with(&ti, WINDOW_TINY, 1, &rt), 0);
    CHECK_EQ(repair_with(&bi, WINDOW_BIG, 64, &rb), 0);
    CHECK(rt.plan.lost_clusters == rb.plan.lost_clusters && rt.plan.trims == rb.plan.trims);
    CHECK(rt.check.passes > 100);
    bool same = fsck_disk_equal(tiny, big);
    CHECK(same);
    if (!same)
        fprintf(stderr, "  %s, seed %" PRIu64 ": the tiny window ends elsewhere\n", c->name,
                c->seed);
    fsck_disk_free(tiny);
    fsck_disk_free(big);

    fault_run_t r = {.name = c->name, .seed = c->seed, .disk = c->d, .repair = run_repair,
                     .repair_ctx = c, .after = no_worse, .clean = clean, .prop_ctx = c,
                     .read_stride = READ_STRIDE};
    fault_result_t res;
    uint32_t failures = fault_run_all(&r, &res);
    CHECK_EQ(failures, 0);
    if (failures)
        damage_print(&c->dm, stderr);
    fault_cases += res.cases;
    fault_runs++;
    cut_points += res.total_writes + 1;         // cut after 0 … total writes
}

// --- the base images ------------------------------------------------------------------------

// Populated FAT32 images, one FAT and two: 0 and 1 small (512-byte clusters, 67808 of them, the
// fewest FAT32 allows at this size), 2 and 3 the harness's (1 KB clusters, about 130000). A
// repair reads the whole FAT three or four times, so the many repairs of the fault runs and the
// random seeds use the small ones; the catalogue runs on all four.
#define BASES          4
#define SMALL_SECTORS  76000u
static fsck_disk_t *base[BASES];
static fsck_fp_t base_fp[BASES];
static damage_catalog_t base_cat[BASES];

static void setup(void) {
    for (int k = 0; k < BASES; k++) {
        BYTE n_fat = (BYTE)(k % 2 + 1);
        base[k] = k < 2 ? fsck_disk_format(SMALL_SECTORS, FM_FAT32, n_fat, SECTOR)
                        : fsck_disk_fat32(n_fat);
        if (!base[k] || !fsck_disk_mount(base[k]) || !fsck_disk_populate())
            abort();
        fsck_disk_unmount();
        CHECK(fsck_fp_take(base[k], &base_fp[k]));
        CHECK(damage_catalog(base[k], &base_cat[k]));
    }
}

static void teardown(void) {
    for (int k = 0; k < BASES; k++) {
        fsck_fp_free(&base_fp[k]);
        damage_catalog_free(&base_cat[k]);
        fsck_disk_free(base[k]);
    }
}

// --- tests ----------------------------------------------------------------------------------

TEST(clean_image) {
    // Nothing to repair: the plan is empty and nothing is written.
    for (int k = 0; k < BASES; k++) {
        fsck_disk_t *d = fsck_disk_copy(base[k]);
        fsck_disk_snapshot(d);
        fsck_io_t io = {.read = fsck_disk_read, .write = fsck_disk_write, .ctx = d,
                        .sectors = fsck_disk_sectors(d)};
        rep_t r;
        CHECK_EQ(repair_with(&io, WINDOW_BIG, 64, &r), 0);
        CHECK(!fsck_plan_writes(&r.plan));
        CHECK_EQ(r.writes, 0);
        CHECK_EQ(r.check.problems, 0);
        uint64_t lba;
        CHECK_EQ(fsck_disk_changed(d, &lba, 1), 0);
        fsck_disk_free(d);
    }
}

TEST(new_refuses) {
    fsck_io_t io = {.read = fsck_disk_read, .ctx = base[0], .sectors = 1000};
    CHECK(fsck_repair_new(&io, mem, sizeof(mem)) == NULL);     // no write callback
    io.write = fsck_disk_write;
    CHECK(fsck_repair_new(&io, mem, fsck_repair_state_size()) == NULL);   // no window
    CHECK(fsck_repair_new(&io, mem, fsck_repair_state_size() + FSCK_WINDOW_MIN + 8) != NULL);
}

// Every kind, alone, on each base image it applies to; on the small ones, then through every
// fault.
TEST(damage_catalogue) {
    static case_t c;
    unsigned cases = 0;
    for (int k = 0; k < BASES; k++) {
        for (unsigned kind = 0; kind < DAMAGE_FAT32_KINDS; kind++) {
            if (kind == DAMAGE_FAT_COPIES && k % 2 == 0)
                continue;                       // needs two FATs
            for (uint64_t seed = 1; seed <= 2; seed++) {
                memset(&c, 0, sizeof(c));
                c.name = damage_fat32_kinds[kind].name;
                c.seed = seed * 1000 + kind;
                c.d = fsck_disk_copy(base[k]);
                c.dm.seed = c.seed;
                c.dm.n = 1;
                CHECK(damage_apply(c.d, &base_cat[k], &damage_fat32_kinds[kind], c.seed,
                                   &c.dm.f[0]));
                case_setup(&c, &base_cat[k], &base_fp[k]);
                if (kind == DAMAGE_OVERLONG_XLINK) {
                    // §10.4: the trim is skipped, and both files read the same.
                    CHECK(c.want.cross_links && c.want.trims == 1);
                    CHECK_EQ(fsck_fp_diff(&base_fp[k], &c.dmg_fp, NULL, 0, stderr), 0);
                }
                repair_case(&c, seed == 1 && k < 2);
                if (kind == DAMAGE_OVERLONG_XLINK)
                    CHECK(c.rep.done.trims == 0 && c.rep.plan.cross_links);
                cases++;
                case_free(&c);
                fsck_disk_free(c.d);
            }
        }
    }
    CHECK_EQ(cases, 2 * 2 * (2 * DAMAGE_FAT32_KINDS - 1));
}

// One to four faults of every kind, the first repairable, over many seeds.
TEST(random_damage) {
    static case_t c;
    unsigned kinds_seen[DAMAGE_FAT32_KINDS] = {0};
    for (uint64_t seed = 1; seed <= SEEDS; seed++) {
        int k = (int)(seed % 2);
        memset(&c, 0, sizeof(c));
        c.name = "random damage";
        c.seed = seed;
        c.d = fsck_disk_copy(base[k]);
        CHECK(damage_random(c.d, &base_cat[k], damage_fat32_kinds, DAMAGE_FAT32_KINDS, seed,
                            &c.dm));
        for (unsigned i = 0; i < c.dm.n; i++)
            kinds_seen[c.dm.f[i].kind - damage_fat32_kinds]++;
        case_setup(&c, &base_cat[k], &base_fp[k]);
        repair_case(&c, seed <= FAULT_SEEDS);
        case_free(&c);
        fsck_disk_free(c.d);
    }
    for (unsigned kind = 0; kind < DAMAGE_FAT32_KINDS; kind++)
        CHECK(kinds_seen[kind] > 0);
    printf("random damage: %u seeds, %u through every fault; fault runs: %" PRIu32 " cases, %" PRIu32
           " faulted repairs (%" PRIu32 " cuts, %" PRIu32 " failed writes, %" PRIu32
           " failed reads)\n", SEEDS, FAULT_SEEDS, fault_runs, fault_cases, cut_points,
           cut_points - fault_runs, fault_cases - 2 * cut_points + fault_runs);
}

// A directory whose chain breaks: FatFs still reads entries the walk can't, so nothing is
// freed or trimmed, even with a lost chain and an over-long file elsewhere.
TEST(unread_directory_frees_nothing) {
    fsck_disk_t *d = fsck_disk_copy(base[0]);
    damage_fat32_t g;
    CHECK(damage_fat32_parse(d, &g));
    damage_fault_t lost, longer;
    CHECK(damage_apply(d, &base_cat[0], &damage_fat32_kinds[DAMAGE_LOST_CHAIN], 5, &lost));
    CHECK(damage_apply(d, &base_cat[0], &damage_fat32_kinds[DAMAGE_OVERLONG], 6, &longer));
    int i = damage_catalog_find(&base_cat[0], "/dir/sub");
    CHECK(i >= 0);
    if (i < 0)
        return;
    uint32_t cl = base_cat[0].e[i].clust;
    damage_fat_set(d, &g, DAMAGE_ALL_COPIES, cl, 0);           // its first cluster: free
    fsck_disk_snapshot(d);
    fsck_io_t io = {.read = fsck_disk_read, .write = fsck_disk_write, .ctx = d,
                    .sectors = fsck_disk_sectors(d)};
    rep_t r;
    CHECK_EQ(repair_with(&io, WINDOW_BIG, 64, &r), 0);
    CHECK(r.plan.unread);
    CHECK(r.plan.lost_clusters >= lost.n_clusters);
    CHECK_EQ(r.plan.trims, 1);
    // Freeing the directory's cluster made FSInfo stale: that is all that is written.
    CHECK(r.plan.fsinfo);
    CHECK_EQ(r.writes, 1);
    CHECK_EQ(r.done.lost_clusters + r.done.trims, 0);
    uint64_t lba[2];
    CHECK_EQ(fsck_disk_changed(d, lba, 2), 1);
    CHECK_EQ(lba[0], g.vol + g.fsinfo);
    CHECK(r.left.unread && r.left.lost_clusters == r.plan.lost_clusters);
    fsck_disk_free(d);
}

// An empty file with a chain isn't trimmed: its entry would have to change.
TEST(empty_file_with_chain) {
    fsck_disk_t *d = fsck_disk_copy(base[0]);
    int i = damage_catalog_find(&base_cat[0], "/var/log/messages");
    CHECK(i >= 0);
    if (i < 0)
        return;
    const damage_entry_t *e = &base_cat[0].e[i];
    uint8_t s[SECTOR];
    fsck_disk_get(d, e->dir_lba, s);
    memset(s + e->dir_ofs + 28, 0, 4);                          // size 0, chain kept
    fsck_disk_put(d, e->dir_lba, s);
    fsck_io_t io = {.read = fsck_disk_read, .write = fsck_disk_write, .ctx = d,
                    .sectors = fsck_disk_sectors(d)};
    rep_t r;
    CHECK_EQ(repair_with(&io, WINDOW_BIG, 64, &r), 0);
    CHECK_EQ(r.plan.empty_chains, 1);
    CHECK_EQ(r.plan.trims, 0);
    CHECK(!fsck_plan_writes(&r.plan));
    CHECK_EQ(r.left.empty_chains, 1);
    fsck_disk_free(d);
}

// exFAT isn't repaired (phase 2b checks it): nothing is written.
TEST(exfat_untouched) {
    fsck_disk_t *d = fsck_disk_format(FSCK_DISK_SMALL_SECTORS, FM_EXFAT, 1, 0);
    CHECK(d != NULL);
    if (!d)
        return;
    fsck_disk_snapshot(d);
    fsck_io_t io = {.read = fsck_disk_read, .write = fsck_disk_write, .ctx = d,
                    .sectors = fsck_disk_sectors(d)};
    rep_t r;
    int rc = repair_with(&io, WINDOW_BIG, 64, &r);
    CHECK(rc != 0 || (r.plan.exfat && !fsck_plan_writes(&r.plan)));
    CHECK_EQ(r.writes, 0);
    uint64_t lba;
    CHECK_EQ(fsck_disk_changed(d, &lba, 1), 0);
    fsck_disk_free(d);
}

// Stopping at the plan writes nothing.
TEST(stop_at_plan) {
    fsck_disk_t *d = fsck_disk_copy(base[1]);
    damage_fault_t f;
    CHECK(damage_apply(d, &base_cat[1], &damage_fat32_kinds[DAMAGE_LOST_CHAIN], 9, &f));
    fsck_disk_snapshot(d);
    fsck_io_t io = {.read = fsck_disk_read, .write = fsck_disk_write, .ctx = d,
                    .sectors = fsck_disk_sectors(d)};
    fsck_repair_t *r = fsck_repair_new(&io, mem, sizeof(mem));
    CHECK(r != NULL);
    if (!r)
        return;
    int st;
    while ((st = fsck_repair_step(r, 8)) == FSCK_MORE)
        ;
    CHECK_EQ(st, FSCK_PLAN);
    CHECK_EQ(fsck_repair_phase(r), FSCK_PHASE_PLAN);
    CHECK_EQ(fsck_repair_plan(r)->lost_clusters, f.n_clusters);
    CHECK(fsck_repair_plan(r)->fsinfo);
    CHECK_EQ(fsck_repair_step(r, 8), FSCK_PLAN);              // waits
    CHECK_EQ(fsck_repair_writes(r), 0);
    uint64_t lba;
    CHECK_EQ(fsck_disk_changed(d, &lba, 1), 0);
    fsck_disk_free(d);
}

TEST(steps_within_budget) {
    CHECK_EQ(step_violations, 0);
}

TEST_MAIN(
    setup();
    RUN(new_refuses);
    RUN(clean_image);
    RUN(stop_at_plan);
    RUN(unread_directory_frees_nothing);
    RUN(empty_file_with_chain);
    RUN(exfat_untouched);
    RUN(damage_catalogue);
    RUN(random_damage);
    RUN(steps_within_budget);
    teardown();
)
