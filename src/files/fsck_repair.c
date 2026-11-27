// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The repairs of `sd check fix` (fsck_repair.h; design in docs/design/sd-check.md §10). The
// checker (fsck.c) does the walking and the accounting; this file decides what is written and
// writes it, from the hooks fsck.c calls, and strings the runs together: check, plan, repair
// run(s), final check.

#include "files/fsck_repair.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define SECTOR          512u
#define FAT_PER_SEC     (SECTOR / 4u)
#define FAT32_MASK      0x0FFFFFFFu
#define FAT32_EOC_MARK  0x0FFFFFFFu     // the end mark FatFs writes
#define FSI_Free_Count  488
#define NO_LBA          UINT64_MAX

enum { PH_CHECK, PH_PLAN, PH_REPAIR, PH_FINAL, PH_DONE, PH_FAILED };

struct fsck_repair {
    fsck_io_t io;               // the caller's: the checks report through it
    fsck_io_t quiet;            // the same without report(), for the repair runs
    fsck_fix_t fix;
    fsck_plan_t plan, done, left;
    fsck_result_t check;
    uint8_t phase;
    bool second;                // the second (one-pass) repair run is running or done
    uint32_t writes;            // in the runs before the one running
    const char *why;
    void *mem;                  // the checker's memory, for each run
    size_t mem_len;
    fsck_t *f;
};

// --- writing --------------------------------------------------------------------------------

static void charge(fsck_fix_t *x) {
    if (*x->budget)
        (*x->budget)--;
}

// One sector. False, with x->why set, if the device refused it.
static bool put(fsck_fix_t *x, uint64_t lba, const uint8_t *buf) {
    if (!x->nfats) {                            // no FAT32 boot sector seen: exFAT, or a bug
        snprintf(x->why, sizeof(x->why), "repair writes only to FAT32");
        return false;
    }
    x->writes++;
    charge(x);
    if (lba >= x->io->sectors || !x->io->write(x->io->ctx, lba, 1, buf)) {
        snprintf(x->why, sizeof(x->why), "write error at sector %" PRIu64, lba);
        return false;
    }
    return true;
}

// FAT sector `lba` (of the first copy) to every copy, the first copy first.
static bool put_fat(fsck_fix_t *x, uint64_t lba, const uint8_t *buf) {
    for (uint32_t k = 0; k < x->nfats; k++)
        if (!put(x, lba + (uint64_t)k * x->fsize, buf))
            return false;
    return true;
}

// Entry `c` of the cached FAT sector, its top 4 bits kept (FatFs's put_fat()).
static void set_entry(uint8_t *fatbuf, uint32_t c, uint32_t v) {
    uint8_t *p = fatbuf + (c % FAT_PER_SEC) * 4;
    v = (v & FAT32_MASK) | ((uint32_t)p[3] << 24 & ~FAT32_MASK);
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

// --- the hooks (fsck_repair.h) --------------------------------------------------------------

static void fix_volume(fsck_fix_t *x, uint64_t fat_lba, uint32_t fsize, uint8_t nfats,
                       uint16_t nrsv, uint32_t *passes) {
    x->fat_lba = fat_lba;
    x->fsize = fsize;
    x->nfats = nfats;
    x->nrsv = nrsv;
    if (x->one_pass)
        *passes = 1;
}

static int fix_fsinfo(fsck_fix_t *x, uint64_t lba, uint8_t *sector) {
    uint32_t v = (uint32_t)sector[FSI_Free_Count] | (uint32_t)sector[FSI_Free_Count + 1] << 8
               | (uint32_t)sector[FSI_Free_Count + 2] << 16
               | (uint32_t)sector[FSI_Free_Count + 3] << 24;
    x->fsi_valid = x->nrsv > 1;                 // else it is FAT sector 0 too: never written
    x->fsi_free = v;
    if (!x->write || !x->fsinfo || !x->fsi_valid || v == UINT32_MAX)
        return FSCK_FIX_OK;
    memset(sector + FSI_Free_Count, 0xFF, 4);
    if (!put(x, lba, sector))
        return FSCK_FIX_FAIL;
    x->fsi_done = true;
    return FSCK_FIX_OK;
}

static int fix_fat_copy(fsck_fix_t *x, uint32_t i, const uint8_t *copy0, const uint8_t *copy1) {
    if (!memcmp(copy0, copy1, SECTOR))
        return FSCK_FIX_OK;
    x->fat_sectors++;
    if (x->write && !put(x, x->fat_lba + x->fsize + i, copy0))
        return FSCK_FIX_FAIL;
    return FSCK_FIX_OK;
}

static int fix_trim(fsck_fix_t *x, uint32_t need, uint32_t n) {
    if (n <= need)
        return FSCK_FIX_OK;
    if (!need) {
        x->empty_chains++;
        return FSCK_FIX_OK;
    }
    if (!x->write) {
        x->trims_n++;
        x->trim_clusters += n - need;
        return FSCK_FIX_OK;
    }
    if (!x->trims)
        return FSCK_FIX_OK;
    if (!*x->budget)
        return FSCK_FIX_YIELD;
    x->pending = n - need;
    return FSCK_FIX_LOAD;
}

static int fix_end_mark(fsck_fix_t *x, uint8_t *fatbuf, uint64_t lba, uint32_t c) {
    set_entry(fatbuf, c, FAT32_EOC_MARK);
    if (!put_fat(x, lba, fatbuf))
        return FSCK_FIX_FAIL;
    x->trims_n++;
    x->trim_clusters += x->pending;
    return FSCK_FIX_OK;
}

static void fix_lost(fsck_fix_t *x, uint8_t *fatbuf, uint64_t lba, uint32_t c) {
    if ((x->write && !x->frees))
        return;
    x->lost++;
    if (!x->write)
        return;
    set_entry(fatbuf, c, 0);
    x->dirty_lba = lba;
}

static int fix_flush(fsck_fix_t *x, const uint8_t *fatbuf, uint64_t next) {
    if (x->dirty_lba == NO_LBA || x->dirty_lba == next)
        return FSCK_FIX_OK;
    if (!*x->budget)
        return FSCK_FIX_YIELD;
    uint64_t lba = x->dirty_lba;
    x->dirty_lba = NO_LBA;
    return put_fat(x, lba, fatbuf) ? FSCK_FIX_OK : FSCK_FIX_FAIL;
}

static const fsck_fix_ops_t fix_ops = {
    .volume = fix_volume, .fsinfo = fix_fsinfo, .fat_copy = fix_fat_copy, .trim = fix_trim,
    .end_mark = fix_end_mark, .lost = fix_lost, .flush = fix_flush,
};

// --- the runs -------------------------------------------------------------------------------

bool fsck_plan_writes(const fsck_plan_t *p) {
    if (p->exfat)
        return false;
    return p->fsinfo || p->fat_sectors || (!p->unread && p->lost_clusters)
        || (!p->unread && !p->cross_links && p->trims);
}

// A counting run's findings as a plan.
static void plan_from(const fsck_fix_t *x, const fsck_result_t *res, fsck_plan_t *p) {
    memset(p, 0, sizeof(*p));
    if (res->kind == FSCK_KIND_EXFAT) {
        p->exfat = true;                        // checked, but repair isn't built yet
        return;
    }
    p->lost_clusters = x->lost;
    p->trims = x->trims_n;
    p->trim_clusters = x->trim_clusters;
    p->empty_chains = x->empty_chains;
    p->fat_sectors = x->fat_sectors;
    p->cross_links = res->cross_linked > 0;
    p->unread = x->unread;
    // FSInfo: invalidated when it is stale, or when anything else is written.
    if (x->fsi_valid && x->fsi_free != UINT32_MAX)
        p->fsinfo = x->fsi_free != res->free_clusters || fsck_plan_writes(p);
}

// A new checker run in the same memory: PH_CHECK and PH_FINAL count, PH_REPAIR writes.
static void start(fsck_repair_t *r, uint8_t phase) {
    bool repair = phase == PH_REPAIR;
    r->f = fsck_new(repair ? &r->quiet : &r->io, r->mem, r->mem_len);
    fsck_fix_t *x = &r->fix;
    r->writes += x->writes;
    memset(x, 0, sizeof(*x));
    x->ops = &fix_ops;
    x->io = &r->io;
    x->dirty_lba = NO_LBA;
    x->write = repair;
    if (repair) {
        const fsck_plan_t *p = &r->plan;
        x->frees = !p->unread;
        x->trims = !r->second && !p->unread && !p->cross_links;
        x->fsinfo = !r->second && p->fsinfo;
        x->one_pass = r->second;
    }
    fsck_set_fix(r->f, x);
    r->phase = phase;
}

static void add_done(fsck_repair_t *r) {
    const fsck_fix_t *x = &r->fix;
    fsck_plan_t *d = &r->done;
    d->lost_clusters += x->lost;
    d->trims += x->trims_n;
    d->trim_clusters += x->trim_clusters;
    d->fat_sectors += x->fat_sectors;
    d->fsinfo = d->fsinfo || x->fsi_done;
}

size_t fsck_repair_state_size(void) {
    return ((sizeof(fsck_repair_t) + 7) & ~(size_t)7) + fsck_state_size();
}

fsck_repair_t *fsck_repair_new(const fsck_io_t *io, void *mem, size_t mem_len) {
    if (!io || !io->read || !io->write || !io->sectors || !mem)
        return NULL;
    size_t pad = (8 - ((uintptr_t)mem & 7)) & 7;
    if (mem_len < pad + fsck_repair_state_size() + FSCK_WINDOW_MIN)
        return NULL;
    fsck_repair_t *r = (fsck_repair_t *)(void *)((uint8_t *)mem + pad);
    memset(r, 0, sizeof(*r));
    size_t own = (sizeof(fsck_repair_t) + 7) & ~(size_t)7;
    r->io = *io;
    r->quiet = *io;
    r->quiet.report = NULL;
    r->mem = (uint8_t *)r + own;
    r->mem_len = mem_len - pad - own;
    start(r, PH_CHECK);
    return r->f ? r : NULL;
}

int fsck_repair_step(fsck_repair_t *r, uint32_t budget) {
    switch (r->phase) {
    case PH_PLAN:
        return FSCK_PLAN;
    case PH_DONE:
        return FSCK_DONE;
    case PH_FAILED:
        return FSCK_FAILED;
    default:
        break;
    }
    int st = fsck_step(r->f, budget);
    if (st == FSCK_MORE)
        return FSCK_MORE;
    if (st == FSCK_FAILED) {
        if (r->phase == PH_REPAIR)
            add_done(r);
        r->writes += r->fix.writes;
        r->fix.writes = 0;
        r->why = fsck_result(r->f)->why;
        r->phase = PH_FAILED;
        return FSCK_FAILED;
    }
    const fsck_result_t *res = fsck_result(r->f);
    switch (r->phase) {
    case PH_CHECK:
        plan_from(&r->fix, res, &r->plan);
        r->check = *res;
        r->phase = PH_PLAN;
        return FSCK_PLAN;
    case PH_REPAIR:
        add_done(r);
        if (!r->second && r->fix.trims_n) {
            r->second = true;                   // window 1's part of the tails: see the header
            start(r, PH_REPAIR);
        } else {
            start(r, PH_FINAL);
        }
        return FSCK_MORE;
    default:                                    // PH_FINAL
        plan_from(&r->fix, res, &r->left);
        r->phase = PH_DONE;
        return FSCK_DONE;
    }
}

void fsck_repair_confirm(fsck_repair_t *r) {
    if (r->phase != PH_PLAN)
        return;
    if (!fsck_plan_writes(&r->plan)) {
        r->left = r->plan;                      // the final check is the first one
        r->phase = PH_DONE;
        return;
    }
    start(r, PH_REPAIR);
}

fsck_phase_t fsck_repair_phase(const fsck_repair_t *r) {
    static const fsck_phase_t phases[] = {
        [PH_CHECK] = FSCK_PHASE_CHECK, [PH_PLAN] = FSCK_PHASE_PLAN,
        [PH_REPAIR] = FSCK_PHASE_REPAIR, [PH_FINAL] = FSCK_PHASE_FINAL,
        [PH_DONE] = FSCK_PHASE_DONE, [PH_FAILED] = FSCK_PHASE_FAILED,
    };
    return phases[r->phase];
}

const fsck_plan_t *fsck_repair_plan(const fsck_repair_t *r) {
    return &r->plan;
}

const fsck_plan_t *fsck_repair_done(const fsck_repair_t *r) {
    return &r->done;
}

const fsck_plan_t *fsck_repair_left(const fsck_repair_t *r) {
    return &r->left;
}

const fsck_result_t *fsck_repair_result(const fsck_repair_t *r) {
    return fsck_result(r->f);
}

const fsck_result_t *fsck_repair_check(const fsck_repair_t *r) {
    return &r->check;
}

uint32_t fsck_repair_writes(const fsck_repair_t *r) {
    return r->writes + r->fix.writes;
}

const char *fsck_repair_why(const fsck_repair_t *r) {
    return r->why;
}
