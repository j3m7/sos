// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The fault layer, the runner and fingerprints (fsck_faults.h).

#include "fsck_faults.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 512u

static void *xrealloc(void *p, size_t size) {
    void *q = realloc(p, size);
    if (!q) {
        fprintf(stderr, "fsck_faults: out of memory\n");
        abort();
    }
    return q;
}

// --- per-sector counts ----------------------------------------------------------------------

typedef struct {
    uint64_t lba;
    uint32_t reads, writes;
    bool used;
} count_t;

struct fault_count_map {
    count_t *slot;
    size_t cap, used;
};

static size_t count_hash(const fault_count_map_t *m, uint64_t lba) {
    return (size_t)((lba * 0x9E3779B97F4A7C15ull) >> 24) & (m->cap - 1);
}

static count_t *count_find(const fault_count_map_t *m, uint64_t lba) {
    size_t i = count_hash(m, lba);
    while (m->slot[i].used) {
        if (m->slot[i].lba == lba)
            return &m->slot[i];
        i = (i + 1) & (m->cap - 1);
    }
    return NULL;
}

static count_t *count_get(fault_count_map_t *m, uint64_t lba) {
    count_t *c = count_find(m, lba);
    if (c)
        return c;
    if (2 * (m->used + 1) > m->cap) {
        count_t *old = m->slot;
        size_t n = m->cap;
        m->cap *= 2;
        m->slot = calloc(m->cap, sizeof(count_t));
        if (!m->slot)
            abort();
        for (size_t i = 0; i < n; i++) {
            if (!old[i].used)
                continue;
            size_t j = count_hash(m, old[i].lba);
            while (m->slot[j].used)
                j = (j + 1) & (m->cap - 1);
            m->slot[j] = old[i];
        }
        free(old);
    }
    size_t i = count_hash(m, lba);
    while (m->slot[i].used)
        i = (i + 1) & (m->cap - 1);
    m->slot[i] = (count_t){.lba = lba, .used = true};
    m->used++;
    return &m->slot[i];
}

static void count_clear(fault_count_map_t *m) {
    if (m->used)
        memset(m->slot, 0, m->cap * sizeof(count_t));
    m->used = 0;
}

// --- the fault layer ------------------------------------------------------------------------

void fault_init(fault_disk_t *f, fsck_disk_t *disk) {
    memset(f, 0, sizeof(*f));
    f->disk = disk;
    f->counts = calloc(1, sizeof(fault_count_map_t));
    if (!f->counts)
        abort();
    f->counts->cap = 256;
    f->counts->slot = calloc(f->counts->cap, sizeof(count_t));
    if (!f->counts->slot)
        abort();
}

void fault_free(fault_disk_t *f) {
    free(f->log);
    if (f->counts)
        free(f->counts->slot);
    free(f->counts);
    memset(f, 0, sizeof(*f));
}

void fault_arm(fault_disk_t *f, fault_mode_t mode, uint32_t n) {
    f->mode = mode;
    f->n = n;
    f->reads = f->writes = f->applied = f->dropped = 0;
    f->cut = f->failed = false;
    f->failed_at = 0;
    f->writes_after_failure = f->reads_after_failure = 0;
    f->log_len = 0;
    count_clear(f->counts);
}

fault_io_t fault_io(fault_disk_t *f) {
    return (fault_io_t){.read = fault_read, .write = fault_write, .ctx = f,
                        .sectors = fsck_disk_sectors(f->disk)};
}

bool fault_read(void *ctx, uint64_t lba, uint32_t count, uint8_t *buf) {
    fault_disk_t *f = ctx;
    if (f->failed)
        f->reads_after_failure++;
    bool fail = false;
    for (uint32_t i = 0; i < count; i++) {
        f->reads++;
        count_get(f->counts, lba + i)->reads++;
        if (f->mode == FAULT_FAIL_READ && !f->failed && f->reads == f->n) {
            f->failed = true;
            f->failed_at = f->reads;
            fail = true;
        }
    }
    if (fail)
        return false;
    return fsck_disk_read(f->disk, lba, count, buf);
}

static void log_write(fault_disk_t *f, uint64_t lba, bool applied, bool after) {
    if (f->log_len == f->log_cap) {
        f->log_cap = f->log_cap ? 2 * f->log_cap : 64;
        f->log = xrealloc(f->log, f->log_cap * sizeof(fault_write_t));
    }
    f->log[f->log_len++] = (fault_write_t){.lba = lba, .seq = f->writes, .applied = applied,
                                           .after_failure = after};
}

bool fault_write(void *ctx, uint64_t lba, uint32_t count, const uint8_t *buf) {
    fault_disk_t *f = ctx;
    if (lba > fsck_disk_sectors(f->disk) || count > fsck_disk_sectors(f->disk) - lba)
        return false;
    // A write after a failure is counted and logged, and never applied.
    if (f->failed) {
        f->writes_after_failure++;
        for (uint32_t i = 0; i < count; i++) {
            f->writes++;
            count_get(f->counts, lba + i)->writes++;
            log_write(f, lba + i, false, true);
        }
        return false;
    }
    // Fail write N: the call that holds write N writes nothing.
    if (f->mode == FAULT_FAIL_WRITE && f->writes < f->n && f->n <= f->writes + count) {
        for (uint32_t i = 0; i < count; i++) {
            f->writes++;
            count_get(f->counts, lba + i)->writes++;
            log_write(f, lba + i, false, false);
        }
        f->failed = true;
        f->failed_at = f->n;
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        f->writes++;
        count_get(f->counts, lba + i)->writes++;
        bool keep = f->mode != FAULT_CUT || f->writes <= f->n;
        if (keep) {
            fsck_disk_put(f->disk, lba + i, buf + (size_t)SECTOR * i);
            f->applied++;
        } else {
            f->dropped++;
            f->cut = true;
        }
        log_write(f, lba + i, keep, false);
    }
    return true;
}

uint32_t fault_reads_of(const fault_disk_t *f, uint64_t lba) {
    const count_t *c = count_find(f->counts, lba);
    return c ? c->reads : 0;
}

uint32_t fault_writes_of(const fault_disk_t *f, uint64_t lba) {
    const count_t *c = count_find(f->counts, lba);
    return c ? c->writes : 0;
}

void fault_print_log(const fault_disk_t *f, FILE *out) {
    fprintf(out, "  write log (%zu writes, %" PRIu32 " reads):\n", f->log_len, f->reads);
    for (size_t i = 0; i < f->log_len; i++) {
        const fault_write_t *w = &f->log[i];
        fprintf(out, "    #%" PRIu32 " lba %" PRIu64 "%s\n", w->seq, w->lba,
                w->after_failure ? " (after the failure)" : w->applied ? "" : " (dropped)");
    }
}

// --- the runner -----------------------------------------------------------------------------

static const char *mode_name(fault_mode_t mode) {
    switch (mode) {
    case FAULT_OFF:
        return "no fault";
    case FAULT_CUT:
        return "cut after";
    case FAULT_FAIL_WRITE:
        return "fail write";
    case FAULT_FAIL_READ:
        return "fail read";
    }
    return "?";
}

static FILE *out_of(const fault_run_t *r) {
    return r->out ? r->out : stderr;
}

static void report_case(const fault_run_t *r, const fault_disk_t *f, fault_mode_t mode,
                        uint32_t n, uint32_t total, const char *why) {
    FILE *out = out_of(r);
    fprintf(out, "fault run %s, seed %" PRIu64 ": %s N=%" PRIu32 " (of %" PRIu32 "): %s\n",
            r->name ? r->name : "?", r->seed, mode_name(mode), n, total, why);
    fault_print_log(f, out);
}

// Repair with nothing armed; `f` keeps the run's log.
static const char *dry(const fault_run_t *r, fault_disk_t *f, FILE *out) {
    fsck_disk_restore(r->disk);
    fault_arm(f, FAULT_OFF, 0);
    fault_io_t io = fault_io(f);
    if (r->repair(&io, r->repair_ctx) != 0)
        return "the dry run's repair failed";
    if (r->clean && !r->clean(r->disk, r->prop_ctx, out))
        return "the image isn't clean after the dry run";
    return NULL;
}

bool fault_dry_run(const fault_run_t *r, fault_result_t *res) {
    fault_disk_t f;
    fault_init(&f, r->disk);
    memset(res, 0, sizeof(*res));
    fsck_disk_snapshot(r->disk);
    const char *why = dry(r, &f, out_of(r));
    res->total_writes = f.writes;
    res->total_reads = f.reads;
    if (why)
        report_case(r, &f, FAULT_OFF, 0, f.writes, why);
    fsck_disk_restore(r->disk);
    fault_free(&f);
    return !why;
}

// The faulted run's writes before the fault match the dry run's.
static bool same_prefix(const fault_disk_t *f, const fault_disk_t *ref, fault_mode_t mode) {
    for (size_t i = 0; i < f->log_len; i++) {
        const fault_write_t *w = &f->log[i];
        bool before = mode == FAULT_CUT ? w->applied : !w->after_failure && w->applied;
        if (!before)
            continue;
        if (i >= ref->log_len || ref->log[i].lba != w->lba)
            return false;
    }
    return true;
}

// One faulted case; NULL if every rule held, else why not.
static const char *one_case(const fault_run_t *r, fault_disk_t *f, const fault_disk_t *ref,
                            fault_mode_t mode, uint32_t n, FILE *out) {
    fsck_disk_restore(r->disk);
    fault_arm(f, mode, n);
    fault_io_t io = fault_io(f);
    int rc = r->repair(&io, r->repair_ctx);
    if (!same_prefix(f, ref, mode))
        return "the writes before the fault differ from the dry run's (repair isn't deterministic)";
    if (mode != FAULT_CUT) {
        if (!f->failed)
            return "the armed failure never happened (repair isn't deterministic)";
        if (rc == 0)
            return "repair reported success after a failed read or write";
        if (f->writes_after_failure)
            return "repair wrote after a failed read or write";
    }
    if (r->after && !r->after(r->disk, r->prop_ctx, out))
        return "the image after the faulted repair breaks the 'after' property";
    // The faulted run's log is what a failure report shows; the second run gets its own layer.
    fault_disk_t again;
    fault_init(&again, r->disk);
    fault_arm(&again, FAULT_OFF, 0);
    fault_io_t io2 = fault_io(&again);
    int rc2 = r->repair(&io2, r->repair_ctx);
    fault_free(&again);
    if (rc2 != 0)
        return "repair run again to the end failed";
    if (r->clean && !r->clean(r->disk, r->prop_ctx, out))
        return "the image after repair is run again isn't clean";
    return NULL;
}

uint32_t fault_run(const fault_run_t *r, fault_mode_t mode, fault_result_t *res) {
    fault_disk_t ref, f;
    fault_init(&ref, r->disk);
    fault_init(&f, r->disk);
    memset(res, 0, sizeof(*res));
    unsigned max_reports = r->max_reports ? r->max_reports : 3;
    fsck_disk_snapshot(r->disk);

    const char *why = dry(r, &ref, out_of(r));
    res->total_writes = ref.writes;
    res->total_reads = ref.reads;
    if (why) {
        report_case(r, &ref, FAULT_OFF, 0, ref.writes, why);
        res->failures = 1;
    } else if (mode != FAULT_OFF) {
        uint32_t total = mode == FAULT_FAIL_READ ? ref.reads : ref.writes;
        for (uint32_t n = mode == FAULT_CUT ? 0 : 1; n <= total; n++) {
            FILE *out = res->failures < max_reports ? out_of(r) : NULL;
            why = one_case(r, &f, &ref, mode, n, out);
            res->cases++;
            if (!why)
                continue;
            if (!res->failures)
                res->first_failure = n;
            if (out)
                report_case(r, &f, mode, n, total, why);
            res->failures++;
        }
    }
    fsck_disk_restore(r->disk);
    fault_free(&ref);
    fault_free(&f);
    return res->failures;
}

uint32_t fault_run_all(const fault_run_t *r, fault_result_t *res) {
    static const fault_mode_t modes[] = {FAULT_CUT, FAULT_FAIL_WRITE, FAULT_FAIL_READ};
    fault_result_t all = {0}, one;
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        fault_run(r, modes[i], &one);
        if (one.failures && !all.failures)
            all.first_failure = one.first_failure;
        all.failures += one.failures;
        all.cases += one.cases;
        all.total_writes = one.total_writes;
        all.total_reads = one.total_reads;
    }
    *res = all;
    return all.failures;
}

// --- fingerprints ---------------------------------------------------------------------------

uint64_t fsck_fnv1a64(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

static fsck_fp_entry_t *fp_add(fsck_fp_t *fp, const char *path) {
    if (fp->n == fp->cap) {
        fp->cap = fp->cap ? 2 * fp->cap : 64;
        fp->e = xrealloc(fp->e, fp->cap * sizeof(fsck_fp_entry_t));
    }
    fsck_fp_entry_t *e = &fp->e[fp->n++];
    memset(e, 0, sizeof(*e));
    snprintf(e->path, sizeof(e->path), "%s", path);
    return e;
}

static bool fp_visit(const char *path, const FILINFO *info, FIL *file, void *ctx) {
    static uint8_t buf[4096];
    fsck_fp_entry_t *e = fp_add(ctx, path);
    e->dir = !file;
    e->read_ok = true;
    e->hash = FSCK_FNV1A64_INIT;
    if (!file)
        return true;
    e->size = info->fsize;
    for (;;) {
        UINT got = 0;
        if (f_read(file, buf, sizeof(buf), &got) != FR_OK) {
            e->read_ok = false;
            break;
        }
        if (!got)
            break;
        e->hash = fsck_fnv1a64(e->hash, buf, got);
    }
    return true;
}

static int fp_cmp(const void *a, const void *b) {
    return strcmp(((const fsck_fp_entry_t *)a)->path, ((const fsck_fp_entry_t *)b)->path);
}

bool fsck_fp_take(fsck_disk_t *disk, fsck_fp_t *fp) {
    memset(fp, 0, sizeof(*fp));
    if (!fsck_disk_mount(disk))
        return false;
    bool ok = fsck_disk_walk(fp_visit, fp);
    fsck_disk_unmount();
    if (fp->n)
        qsort(fp->e, fp->n, sizeof(fsck_fp_entry_t), fp_cmp);
    return ok;
}

void fsck_fp_free(fsck_fp_t *fp) {
    free(fp->e);
    memset(fp, 0, sizeof(*fp));
}

static bool excluded(const char *path, const char *const *exclude, size_t n) {
    for (size_t i = 0; i < n; i++) {
        size_t len = strlen(exclude[i]);
        if (len && exclude[i][len - 1] == '/' ? !strncmp(path, exclude[i], len)
                                              : !strcmp(path, exclude[i]))
            return true;
    }
    return false;
}

size_t fsck_fp_diff(const fsck_fp_t *a, const fsck_fp_t *b, const char *const *exclude,
                    size_t n_exclude, FILE *out) {
    size_t i = 0, j = 0, diffs = 0;
    while (i < a->n || j < b->n) {
        int c = i == a->n ? 1 : j == b->n ? -1 : strcmp(a->e[i].path, b->e[j].path);
        const fsck_fp_entry_t *x = c <= 0 ? &a->e[i] : NULL, *y = c >= 0 ? &b->e[j] : NULL;
        const fsck_fp_entry_t *any = x ? x : y;
        if (!any)
            break;
        const char *path = any->path;
        const char *what = NULL;
        if (!y)
            what = "only in the first";
        else if (!x)
            what = "only in the second";
        else if (x->dir != y->dir)
            what = "file in one, directory in the other";
        else if (x->size != y->size)
            what = "size differs";
        else if (x->read_ok != y->read_ok)
            what = "reads in one, read error in the other";
        else if (x->hash != y->hash)
            what = "bytes differ";
        if (what && !excluded(path, exclude, n_exclude)) {
            diffs++;
            if (out)
                fprintf(out, "  %s: %s\n", path, what);
        }
        if (x)
            i++;
        if (y)
            j++;
    }
    return diffs;
}

static bool walk_ok(const char *path, const FILINFO *info, FIL *file, void *ctx) {
    (void)path;
    (void)info;
    (void)file;
    (void)ctx;
    return true;
}

bool fsck_fp_write_probe(fsck_disk_t *disk, const char *path, uint32_t size) {
    static uint8_t buf[4096];
    if (!fsck_disk_mount(disk))
        return false;
    bool ok = fsck_disk_put_file(path, size, 0x5EED);
    FIL fp;
    if (ok && f_open(&fp, path, FA_READ) == FR_OK) {
        for (uint32_t done = 0; ok && done < size;) {
            UINT got = 0;
            ok = f_read(&fp, buf, sizeof(buf), &got) == FR_OK && got > 0;
            for (UINT k = 0; ok && k < got; k++)
                ok = buf[k] == fsck_disk_pattern(path, 0x5EED, done + k);
            done += got;
        }
        ok = f_close(&fp) == FR_OK && ok;
    } else {
        ok = false;
    }
    ok = ok && fsck_disk_walk(walk_ok, NULL);
    fsck_disk_unmount();
    return ok;
}
