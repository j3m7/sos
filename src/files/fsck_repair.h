// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// fsck_repair: the repairs of `sd check fix` (docs/design/sd-check.md §10) on the checker of
// fsck.h. Pure C like the checker: sectors are read and written through fsck_io_t, all memory
// comes from the caller, and the work is done a bounded number of sectors at a time, so the
// device can run it as a shell job (phase 5).
//
// A repair is a sequence of checker runs over the same memory:
//   1. the check: an ordinary check (no writes), reported through io->report. What it found is
//      the plan (fsck_repair_plan()). fsck_repair_step() returns FSCK_PLAN and waits;
//   2. the caller shows the plan and asks; fsck_repair_confirm() goes on, or the caller stops;
//   3. the repair run: the checker again, with the writes of §10.2 at its hook points, and
//      nothing reported. When it trimmed chains, a second repair run of one pass follows (below);
//   4. the final check, reported like the first. Its plan (fsck_repair_left()) must have
//      nothing left to do of what was allowed.
//
// FAT32:
// - Lost clusters are freed: FAT entry 0 (its top 4 bits kept, as FatFs keeps them), in the
//   pass whose window holds them. Bad-cluster marks are never lost, so never freed.
// - A file whose chain ends properly but is longer than the file needs gets an end mark at its
//   last needed cluster, in pass 1 when the walk reaches it; the rest of the chain is then lost
//   clusters, freed as above. Directories are never trimmed (their size is 0), nor are empty
//   files with a chain (their entry would have to change: counted in empty_chains, left).
// - FSInfo's free count is set to 0xFFFFFFFF (unknown) before any other write, when FSInfo is
//   valid, inside the reserved area, not already unknown, and either stale or the repair writes
//   anything else. FatFs then counts free clusters again.
// - With two FATs, every FAT sector written goes to the first copy, then the same bytes to the
//   second, as FatFs's sync_window() writes them. A sector where the second copy differs from
//   the first is copied from the first in pass 1: FatFs reads only the first copy, so the
//   second is what FatFs would overwrite at its next write there.
//
// exFAT (the allocation bitmap, not the FAT, says what is in use; no FSInfo, one FAT):
// - At the end of each pass, every bitmap sector that covers the window is compared with the
//   clusters reached: bits of clusters reached but clear are set (reached_free), bits of lost
//   clusters cleared (lost_clusters). The sector's new state, sets and clears together, goes in
//   one write, to the sector it was read from (the bitmap read contiguously from its first
//   cluster, as FatFs reads it), and only when it changed. A sector that covers two windows is
//   written once in each pass. Bits past the last cluster are left as they are. FAT entries of
//   freed clusters are left as they are: FatFs doesn't read them.
// - Bits are set even when frees are skipped (see the rules): setting one only marks a cluster
//   something reaches as used.
// - A file with a FAT chain longer than it needs gets the end mark 0xFFFFFFFF (all 32 bits) at
//   its last needed cluster, in pass 1, as on FAT32; its tail is then lost, and its bits are
//   cleared as above. A NoFatChain file's clusters past its run are lost clusters already: no
//   trim, only their bits cleared. Directories, the bitmap and the up-case table aren't trimmed.
// - Write order (§10.2): a bitmap sector's sets and clears are one write, so no clear in it
//   lands before a set, and a stop leaves the sector as it was or repaired (for the window).
//   Each bit changed takes one cluster from a problem to none (a set only for a cluster
//   reached, a clear only for one nothing reaches), whatever the other bits, so any prefix of
//   the writes, in any order of sectors, leaves a subset of the problems.
// - A cluster marked bad in the FAT and set in the bitmap is lost like any other (the checker
//   doesn't count bad clusters on exFAT), and its bit is cleared. FatFs never marks one.
//
// Rules (beside §10.2's):
// - Trims only when the check found no cross-linked cluster, and neither trims nor frees when
//   the check couldn't walk all of the tree FatFs can read: a directory whose chain loops or
//   breaks (FatFs still reads the entries up to the break, and the cluster it breaks on), a
//   directory nested deeper than FSCK_DEPTH_MAX, or a walk stopped for overlapping chains. What
//   such a directory holds would look lost. On exFAT also no frees when any chain (a file's too)
//   breaks on a free or bad FAT entry: FatFs reads the cluster it breaks on, which the checker
//   doesn't count, and clearing its bit would let FatFs hand it to another file.
// - The first repair run marks a trimmed chain's tail in pass 1 before the trim is known, so the
//   tail's clusters in window 1 aren't freed by it. A second repair run of pass 1 alone frees
//   them (it only frees: nothing else is left to do). Without trims there is one repair run.
// - Every write is one sector (io->write with count 1). A failed read or write stops the repair
//   at once: FSCK_FAILED, no further write, fsck_repair_why() says which sector.
// - Stopping at any write leaves the volume no worse (§10.2): a check reports a subset of the
//   first check's problems, plus at most (a) a "FAT copies differ" note, and (b) lost clusters
//   that are the tail of a chain being trimmed: its end mark was written and the tail not yet
//   freed, which costs space, not data, and running repair again frees it. Every file still
//   reads the same bytes. A subset counts clusters, not lines: a lost or reached-but-free run
//   that crosses a bitmap sector or a window may be left in part, and is then reported shorter.
// - Budget: fsck_repair_step()'s budget counts sectors read and written alike, one unit each.
//   A write that must follow its read (FSInfo, a FAT sector copied to the second FAT, an exFAT
//   bitmap sector) or the other copy of a FAT write may go past it, so a step transfers at most
//   budget + 2 sectors and reads at most budget. A step ends with a run: the next run starts at the next step.

#ifndef SOS_FSCK_REPAIR_H
#define SOS_FSCK_REPAIR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "files/fsck.h"

enum { FSCK_PLAN = FSCK_FAILED + 1 };   // fsck_repair_step(): the check is done, see the plan

// What a check found that repair fixes, and what it will leave. The plan is the first check's;
// fsck_repair_left() is the final check's; fsck_repair_done() counts what the repair runs did.
typedef struct {
    uint32_t lost_clusters;     // lost clusters (done: every cluster freed, trimmed tails too;
                                // exFAT: bitmap bits cleared)
    uint32_t reached_free;      // exFAT: clusters reached but clear in the bitmap (bits set)
    uint32_t trims;             // files whose chain is longer than they need
    uint32_t trim_clusters;     // the clusters past those files' ends
    uint32_t empty_chains;      // empty files with a chain: left alone
    uint32_t fat_sectors;       // sectors where the second FAT differs from the first
    bool fsinfo;                // FSInfo's free count is (done: was) set to unknown
    bool cross_links;           // cross-linked clusters: trims are skipped
    bool unread;                // part of the tree wasn't walked (or, on exFAT, a chain breaks
                                // on a free or bad entry): frees and trims are skipped
} fsck_plan_t;

typedef enum {
    FSCK_PHASE_CHECK,           // the first check
    FSCK_PHASE_PLAN,            // waiting for fsck_repair_confirm()
    FSCK_PHASE_REPAIR,          // a repair run
    FSCK_PHASE_FINAL,           // the final check
    FSCK_PHASE_DONE,
    FSCK_PHASE_FAILED,
} fsck_phase_t;

typedef struct fsck_repair fsck_repair_t;

// The fixed part of repair's memory: fsck_state_size() and about half a KB more (560 B on
// ARM, 632 B on the host). The rest of `mem` is the cluster window, as for fsck_new().
size_t fsck_repair_state_size(void);

// Set up a repair of the device `io` describes (`io` is copied; it needs a write callback) in
// `mem`, which must hold fsck_repair_state_size() + FSCK_WINDOW_MIN bytes. NULL if it doesn't,
// or if `io` lacks a read or write callback or a size.
fsck_repair_t *fsck_repair_new(const fsck_io_t *io, void *mem, size_t mem_len);

// Do a bounded amount of work (see "Budget"). FSCK_MORE: call again. FSCK_PLAN: the check is
// done; read the plan, then fsck_repair_confirm() and call again, or stop here (nothing has been
// written). FSCK_DONE: finished, see fsck_repair_result() (the final check) and
// fsck_repair_left(). FSCK_FAILED: a read or write failed, or the check couldn't go on; see
// fsck_repair_why(). After DONE or FAILED it returns the same again without doing anything.
int fsck_repair_step(fsck_repair_t *r, uint32_t budget);

// Go on from FSCK_PLAN. With nothing to write (fsck_plan_writes() false) the repair is done at
// once and the final check is the first one.
void fsck_repair_confirm(fsck_repair_t *r);

fsck_phase_t fsck_repair_phase(const fsck_repair_t *r);
// The plan: valid from FSCK_PLAN on.
const fsck_plan_t *fsck_repair_plan(const fsck_repair_t *r);
// What the repair runs did so far.
const fsck_plan_t *fsck_repair_done(const fsck_repair_t *r);
// What the final check found still to repair: valid once FSCK_DONE.
const fsck_plan_t *fsck_repair_left(const fsck_repair_t *r);
// The result of the running or last run: the first check's until a repair run starts, then the
// repair run's (for its pass and passes), then the final check's.
const fsck_result_t *fsck_repair_result(const fsck_repair_t *r);
// The first check's result, kept once FSCK_PLAN.
const fsck_result_t *fsck_repair_check(const fsck_repair_t *r);
// Sectors written so far.
uint32_t fsck_repair_writes(const fsck_repair_t *r);
// FSCK_FAILED: why ("read error at sector N", "write error at sector N", or the check's).
const char *fsck_repair_why(const fsck_repair_t *r);

// True if repairing per `p` writes anything.
bool fsck_plan_writes(const fsck_plan_t *p);

// --- between fsck.c and fsck_repair.c -------------------------------------------------------
//
// A checker run with a fix attached (fsck_set_fix()) calls these at its hook points; with none
// (a plain check) fsck.c calls them with NULL and they do nothing. In counting mode they count
// what a repair would do and write nothing; in writing mode they write what the run allows.
//
// exFAT: the same hooks for the trim (the end mark in exFAT's 32-bit entries, one FAT), and
// fsck_fix_bitmap() for each bitmap sector compared; no FSInfo, no second FAT, no flush.

enum { FSCK_FIX_OK, FSCK_FIX_YIELD, FSCK_FIX_FAIL, FSCK_FIX_LOAD };

typedef struct fsck_fix fsck_fix_t;

// The hooks, filled in by fsck_repair.c: fsck.c calls them through the wrappers below, so a
// plain check links without this file.
typedef struct {
    void (*volume)(fsck_fix_t *x, fsck_kind_t kind, uint64_t fat_lba, uint32_t fsize,
                   uint8_t nfats, uint16_t nrsv, uint32_t *passes);
    int (*fsinfo)(fsck_fix_t *x, uint64_t lba, uint8_t *sector);
    int (*fat_copy)(fsck_fix_t *x, uint32_t i, const uint8_t *copy0, const uint8_t *copy1);
    int (*trim)(fsck_fix_t *x, uint32_t need, uint32_t n);
    int (*end_mark)(fsck_fix_t *x, uint8_t *fatbuf, uint64_t lba, uint32_t c);
    void (*lost)(fsck_fix_t *x, uint8_t *fatbuf, uint64_t lba, uint32_t c);
    int (*flush)(fsck_fix_t *x, const uint8_t *fatbuf, uint64_t next);
    int (*bitmap)(fsck_fix_t *x, uint64_t lba, uint8_t *sector, uint32_t ofs,
                  const uint8_t *reached, uint32_t bits);
} fsck_fix_ops_t;

struct fsck_fix {
    // Set by fsck_repair.c before a run.
    const fsck_fix_ops_t *ops;
    const fsck_io_t *io;
    bool write;                 // false: count only
    bool frees, trims, fsinfo;  // writing mode: what this run may do
    bool one_pass;              // stop after pass 1
    // Set by fsck.c.
    uint32_t *budget;           // the step's budget: a unit per sector written
    // Set by the volume hook.
    fsck_kind_t kind;           // FSCK_KIND_NONE until it is called: nothing is written
    uint64_t fat_lba;
    uint32_t fsize;
    uint16_t nrsv;
    uint8_t nfats;
    // Found (counting) or done (writing) in this run.
    bool unread;
    bool fsi_valid;
    bool fsi_done;
    uint32_t fsi_free;
    uint32_t lost, trims_n, trim_clusters, empty_chains, fat_sectors;
    uint32_t reached_free;      // exFAT: bits of clusters reached but free (set)
    uint32_t pending;           // the clusters past the end of the chain being trimmed
    uint32_t writes;
    uint64_t dirty_lba;         // the FAT sector (in the checker's cache) with frees not written
    char why[48];
};

// fsck.c: attach `fix` to a run made by fsck_new() (before its first step).
void fsck_set_fix(fsck_t *f, fsck_fix_t *fix);

// The volume, once its boot sector is read (exFAT: nfats 1, nrsv 0); caps *passes at 1 for a
// one-pass run.
static inline void fsck_fix_volume(fsck_fix_t *x, fsck_kind_t kind, uint64_t fat_lba,
                                   uint32_t fsize, uint8_t nfats, uint16_t nrsv,
                                   uint32_t *passes) {
    if (x)
        x->ops->volume(x, kind, fat_lba, fsize, nfats, nrsv, passes);
}

// FSInfo with valid signatures was read into `sector` (lba): may set its free count unknown.
static inline int fsck_fix_fsinfo(fsck_fix_t *x, uint64_t lba, uint8_t *sector) {
    return x ? x->ops->fsinfo(x, lba, sector) : FSCK_FIX_OK;
}

// FAT sector `i` of both copies compared: a differing second copy gets the first's bytes.
static inline int fsck_fix_fat_copy(fsck_fix_t *x, uint32_t i, const uint8_t *copy0,
                                    const uint8_t *copy1) {
    return x ? x->ops->fat_copy(x, i, copy0, copy1) : FSCK_FIX_OK;
}

// A directory FatFs reads wasn't walked (whole), or the walk stopped.
static inline void fsck_fix_unread(fsck_fix_t *x) {
    if (x)
        x->unread = true;
}

// Pass 1: a file's chain ended properly with `n` clusters, and the file needs `need`.
// FSCK_FIX_LOAD: load the FAT sector of its last needed cluster into the cache, then call
// fsck_fix_end_mark(). FSCK_FIX_YIELD: no budget left, call again.
static inline int fsck_fix_trim(fsck_fix_t *x, uint32_t need, uint32_t n) {
    return x ? x->ops->trim(x, need, n) : FSCK_FIX_OK;
}

// The end mark at cluster `c` (FAT32's or exFAT's, by the volume's kind), in the cached FAT
// sector `fatbuf` (sector `lba`), written.
static inline int fsck_fix_end_mark(fsck_fix_t *x, uint8_t *fatbuf, uint64_t lba, uint32_t c) {
    return x ? x->ops->end_mark(x, fatbuf, lba, c) : FSCK_FIX_OK;
}

// Cluster `c` is lost: its entry in the cached FAT sector is set free, written by
// fsck_fix_flush().
static inline void fsck_fix_lost(fsck_fix_t *x, uint8_t *fatbuf, uint64_t lba, uint32_t c) {
    if (x)
        x->ops->lost(x, fatbuf, lba, c);
}

// Before the cache loads sector `next` (or the pass ends: next UINT64_MAX), the freed entries
// in it are written.
static inline int fsck_fix_flush(fsck_fix_t *x, const uint8_t *fatbuf, uint64_t next) {
    return x ? x->ops->flush(x, fatbuf, next) : FSCK_FIX_OK;
}

// exFAT, the end of a pass: bitmap sector `lba` was read into `sector`, and its bits from byte
// `ofs` on stand for `bits` clusters the window `reached` (byte-aligned, bit 0 first) covers.
// Counts the bits to set and to clear; writing, changes them in `sector` and writes it, once.
static inline int fsck_fix_bitmap(fsck_fix_t *x, uint64_t lba, uint8_t *sector, uint32_t ofs,
                                  const uint8_t *reached, uint32_t bits) {
    return x ? x->ops->bitmap(x, lba, sector, ofs, reached, bits) : FSCK_FIX_OK;
}

// After FSCK_FIX_FAIL: "write error at sector N".
static inline const char *fsck_fix_why(const fsck_fix_t *x) {
    return x->why;
}

#endif
