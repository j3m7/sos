// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// src/files/fsck.c: the card checker reads whatever is on a card, so every byte of the volume
// is untrusted. The input is one selector byte, then a sparse image (fsck_image.h). The image
// is checked twice, with two different window sizes and step budgets, and the two results
// must agree: the same counts and, when every line fitted under FSCK_REPORT_MAX, the same
// lines. That holds the multi-pass accounting and the resumable steps to one answer. Each run
// must also end: more than RUN_READS_MAX reads is taken for a hang.
//
// With the selector's top bit set, the image is then repaired (fsck_repair.h) on a copy-on-write
// disk over it (the image stays as it was), with the first run's window and budget: a repair
// mustn't crash or hang either, no step may read past its budget or transfer more than
// budget + 2 sectors, every write is one sector, and a repair that finishes leaves nothing its
// final check would repair again. It only writes sectors that hold data, or their second FAT
// copy, so the overlay never fills.

#include <stdlib.h>
#include <string.h>

#include "files/fsck.h"
#include "files/fsck_repair.h"
#include "fsck_image.h"
#include "fuzz.h"

#define RUN_READS_MAX  20000000u

typedef struct {
    int status;
    fsck_result_t res;
    unsigned n;
    uint32_t reads;
    char lines[FSCK_REPORT_MAX][FSCK_LINE_MAX];
} run_t;

static fsck_image_t image;
static run_t runs[2];
static uint8_t mem[16384 + 16384];

// The copy-on-write disk: sectors repair wrote, over the image.
#define OVERLAY_MAX 1024u
static struct {
    unsigned n;
    uint32_t reads, writes;     // in all, and in the current step
    uint32_t step_reads, step_writes;
    uint64_t lba[OVERLAY_MAX];
    uint8_t data[OVERLAY_MAX][512];
} cow;

// Windows from 2 KB (16384 clusters a pass) up to the board's 16 KB: smaller ones would only
// multiply the passes over the same walk.
static const size_t windows[4] = {2048, 4096, 8192, 16384};

static bool read_fn(void *ctx, uint64_t lba, uint32_t count, uint8_t *buf) {
    run_t *r = ctx;
    if (++r->reads > RUN_READS_MAX)
        abort();
    return fsck_image_read(&image, lba, count, buf);
}

static void report(void *ctx, const char *line) {
    run_t *r = ctx;
    size_t len = strnlen(line, FSCK_LINE_MAX);
    if (len == FSCK_LINE_MAX || r->n >= FSCK_REPORT_MAX)
        abort();                                // unterminated, or past the report limit
    memcpy(r->lines[r->n++], line, len + 1);
}

static int cmp_line(const void *a, const void *b) {
    return strcmp(a, b);
}

static void run(run_t *r, size_t window, uint32_t budget) {
    r->n = 0;
    r->reads = 0;
    fsck_io_t io = {read_fn, report, NULL, r, image.sectors};
    if (sizeof(mem) < fsck_state_size() + window)
        abort();
    fsck_t *f = fsck_new(&io, mem, fsck_state_size() + window);
    if (!f)
        abort();
    uint32_t before;
    do {
        before = r->reads;
        r->status = fsck_step(f, budget);
        if (r->reads - before > budget)
            abort();                            // a step read more than its budget
    } while (r->status == FSCK_MORE);
    r->res = *fsck_result(f);
    const fsck_result_t *res = &r->res;
    if (res->reported != r->n || res->reported > res->problems + res->notes)
        abort();
    if ((r->status == FSCK_FAILED) != (res->why != NULL))
        abort();
    if (r->status == FSCK_DONE && (!res->summary[0] || res->pass != res->passes))
        abort();
    qsort(r->lines, r->n, sizeof(r->lines[0]), cmp_line);
}

static uint8_t *cow_sector(uint64_t lba) {
    for (unsigned i = 0; i < cow.n; i++)
        if (cow.lba[i] == lba)
            return cow.data[i];
    return NULL;
}

static bool cow_read(void *ctx, uint64_t lba, uint32_t count, uint8_t *buf) {
    (void)ctx;
    if ((cow.reads += count) > 5 * RUN_READS_MAX)
        abort();                                // a repair is four or five runs at most
    cow.step_reads += count;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *s = cow_sector(lba + i);
        if (s)
            memcpy(buf + 512 * i, s, 512);
        else if (!fsck_image_read(&image, lba + i, 1, buf + 512 * i))
            return false;
    }
    return true;
}

static bool cow_write(void *ctx, uint64_t lba, uint32_t count, const uint8_t *buf) {
    (void)ctx;
    if (count != 1 || lba >= image.sectors)
        abort();                                // repair writes single sectors on the device
    cow.writes++;
    cow.step_writes++;
    uint8_t *s = cow_sector(lba);
    if (!s) {
        if (cow.n == OVERLAY_MAX)
            abort();                            // it wrote sectors that held nothing
        cow.lba[cow.n] = lba;
        s = cow.data[cow.n++];
    }
    memcpy(s, buf, 512);
    return true;
}

static void cow_report(void *ctx, const char *line) {
    (void)ctx;
    if (strnlen(line, FSCK_LINE_MAX) == FSCK_LINE_MAX)
        abort();
}

static void repair(size_t window, uint32_t budget) {
    cow.n = 0;
    cow.reads = cow.writes = 0;
    fsck_io_t io = {cow_read, cow_report, cow_write, NULL, image.sectors};
    if (sizeof(mem) < fsck_repair_state_size() + window)
        abort();
    fsck_repair_t *r = fsck_repair_new(&io, mem, fsck_repair_state_size() + window);
    if (!r)
        abort();
    int st;
    do {
        cow.step_reads = cow.step_writes = 0;
        st = fsck_repair_step(r, budget);
        if (cow.step_reads > budget || cow.step_reads + cow.step_writes > budget + 2)
            abort();
        if (st == FSCK_PLAN) {
            if (cow.writes)
                abort();                        // the check wrote
            fsck_repair_confirm(r);
            st = FSCK_MORE;
        }
    } while (st == FSCK_MORE);
    if (fsck_repair_writes(r) != cow.writes)
        abort();
    if (st == FSCK_FAILED) {
        // No read or write fails here, so only the first check can fail (no volume, a boot
        // sector FatFs refuses, exFAT), before anything is written.
        if (fsck_repair_phase(r) != FSCK_PHASE_FAILED || cow.writes || !fsck_repair_why(r))
            abort();
        return;
    }
    if (fsck_plan_writes(fsck_repair_left(r)))
        abort();                                // something repairable is left
    if (!fsck_plan_writes(fsck_repair_plan(r)) && cow.writes)
        abort();                                // nothing planned, yet it wrote
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    uint8_t sel = fuzz_byte(&data, &size);
    fsck_image_parse(&image, data, size);
    if (!image.sectors)
        return 0;
    unsigned w0 = sel & 3, w1 = (w0 + 1 + (sel >> 2 & 3) % 3) & 3;
    run(&runs[0], windows[w0], 1 + (sel >> 4 & 7));
    run(&runs[1], windows[w1], 64);
    const fsck_result_t *a = &runs[0].res, *b = &runs[1].res;
    if (runs[0].status != runs[1].status || a->problems != b->problems || a->notes != b->notes
            || a->lost_clusters != b->lost_clusters || a->cross_linked != b->cross_linked
            || a->free_clusters != b->free_clusters || a->bad_clusters != b->bad_clusters
            || a->files != b->files || a->dirs != b->dirs || strcmp(a->summary, b->summary))
        abort();
    if ((a->why || b->why) && (!a->why || !b->why || strcmp(a->why, b->why)))
        abort();
    if (a->problems + a->notes < FSCK_REPORT_MAX)
        for (unsigned i = 0; i < runs[0].n; i++)
            if (strcmp(runs[0].lines[i], runs[1].lines[i]))
                abort();
    if (sel & 0x80)
        repair(windows[w0], 1 + (sel >> 4 & 7));
    return 0;
}
