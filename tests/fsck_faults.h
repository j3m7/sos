// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Fault injection for testing the card checker's repair (docs/design/sd-check.md §10.4):
//
// - a fault layer: fsck_io_t-shaped read and write callbacks over an fsck_disk_t that count
//   every sector read and written, log the writes, and can be armed to cut power after N
//   writes, fail write N or fail read N;
// - a runner that repairs a damaged image once to count its writes, then again for every N
//   with each fault armed, checking a property after the faulted repair and another after
//   repair is run again to the end;
// - fingerprints of every file's path, size and bytes (FNV-1a 64), read through host FatFs, to
//   check that files the damage didn't touch read the same.
//
// Counting is per sector: a call writing 3 sectors is writes N+1, N+2 and N+3. Repair (§10.2)
// writes single sectors, so a call and a sector are the same there.

#ifndef SOS_FSCK_FAULTS_H
#define SOS_FSCK_FAULTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "fsck_disk.h"

// The shape of fsck_io_t (src/files/fsck.h, design note §3), copied so the harness builds
// without fsck.h. Swap it for fsck_io_t when repair lands: the fields are the same.
typedef struct {
    bool (*read)(void *ctx, uint64_t lba, uint32_t count, uint8_t *buf);
    void (*report)(void *ctx, const char *line);
    bool (*write)(void *ctx, uint64_t lba, uint32_t count, const uint8_t *buf);
    void *ctx;
    uint64_t sectors;
} fault_io_t;

// --- the fault layer ------------------------------------------------------------------------

typedef enum {
    FAULT_OFF,          // nothing fails
    FAULT_CUT,          // power cut after `n` writes: later writes are dropped, reported OK
    FAULT_FAIL_WRITE,   // write `n` (from 1) fails: its call returns false, writes nothing
    FAULT_FAIL_READ,    // read `n` (from 1) fails: its call returns false
} fault_mode_t;

typedef struct {
    uint64_t lba;
    uint32_t seq;           // the write's number, from 1
    bool applied;           // reached the disk
    bool after_failure;     // written after a failed read or write
} fault_write_t;

typedef struct fault_count_map fault_count_map_t;

typedef struct {
    fsck_disk_t *disk;
    fault_mode_t mode;
    uint32_t n;
    uint32_t reads, writes;     // sectors read and written (attempted), since fault_arm()
    uint32_t applied, dropped;  // writes that reached the disk; writes lost to the cut
    bool cut;                   // the cut dropped at least one write
    bool failed;                // the armed read or write failure fired
    uint32_t failed_at;         // which read or write it was
    uint32_t writes_after_failure;  // write calls after it (§10.3: there must be none)
    uint32_t reads_after_failure;
    fault_write_t *log;         // every write, in order
    size_t log_len, log_cap;
    fault_count_map_t *counts;  // reads and writes of each sector
} fault_disk_t;

void fault_init(fault_disk_t *f, fsck_disk_t *disk);
void fault_free(fault_disk_t *f);
// Arm a fault (FAULT_OFF: none) and clear the counters, the log and the per-sector counts.
void fault_arm(fault_disk_t *f, fault_mode_t mode, uint32_t n);
// fault_read and fault_write as an fault_io_t (report NULL), ctx `f`.
fault_io_t fault_io(fault_disk_t *f);
bool fault_read(void *ctx, uint64_t lba, uint32_t count, uint8_t *buf);
bool fault_write(void *ctx, uint64_t lba, uint32_t count, const uint8_t *buf);
// Reads and writes of one sector since fault_arm().
uint32_t fault_reads_of(const fault_disk_t *f, uint64_t lba);
uint32_t fault_writes_of(const fault_disk_t *f, uint64_t lba);
// The write log, one write per entry: "  #3 lba 2081 (dropped)".
void fault_print_log(const fault_disk_t *f, FILE *out);

// --- the runner -----------------------------------------------------------------------------

// The repair under test: 0 when it finished, anything else when it stopped (it must stop on a
// failed read or write). It reads and writes only through `io`.
typedef int (*fault_repair_fn)(const fault_io_t *io, void *ctx);
// A property of the image after a run; false fails the case. It may print why to `out`, which
// is NULL once the run has reported enough failures. The image is unmounted; the property may
// mount it (fsck_disk_mount()), and must unmount it. `after` must not change the image (the
// second repair runs on it); `clean` may (fsck_fp_write_probe()): the next case rewinds it.
typedef bool (*fault_prop_fn)(fsck_disk_t *disk, void *ctx, FILE *out);

typedef struct {
    const char *name;           // printed on failure
    uint64_t seed;              // printed on failure (the damage generator's seed)
    // The damaged image. The runner takes a snapshot of it as it is (replacing any snapshot
    // the caller kept), rewinds to it before every case, and leaves it so at the end.
    fsck_disk_t *disk;
    fault_repair_fn repair;
    void *repair_ctx;
    // After a cut or failed repair: §10.2's guarantee ("no worse than before"). NULL: none.
    fault_prop_fn after;
    // After repair is run again to the end (and after the dry run): clean of the kinds repair
    // fixes. NULL: none.
    fault_prop_fn clean;
    void *prop_ctx;
    FILE *out;                  // failure reports; NULL: stderr
    unsigned max_reports;       // failing cases reported in full (0: 3)
} fault_run_t;

typedef struct {
    uint32_t total_writes;      // in the dry run
    uint32_t total_reads;
    uint32_t cases;             // faulted runs done
    uint32_t failures;          // cases that broke a rule
    uint32_t first_failure;     // N of the first failing case (valid when failures > 0)
} fault_result_t;

// The dry run alone: repair with nothing armed, check `clean`, restore. Fills total_writes and
// total_reads. False if repair failed or `clean` didn't hold.
bool fault_dry_run(const fault_run_t *r, fault_result_t *res);

// For every N (cut: 0 … total writes; fail write: 1 … total writes; fail read: 1 … total
// reads): restore, arm, repair, check, then disarm, repair again to the end and check `clean`.
// The rules, beside the two properties:
// - a faulted run's writes before the fault go to the same sectors, in the same order, as the
//   dry run's (repair is deterministic, so "after N writes" means the same thing every time);
// - fail write and fail read: repair returns non-zero, and makes no write call after the
//   failure;
// - the second repair finishes (returns 0).
// Each failing case prints the mode, N, the seed, why, and the write log. Returns the number
// of failing cases (0: all held); the dry run's failure counts as one.
uint32_t fault_run(const fault_run_t *r, fault_mode_t mode, fault_result_t *res);
// All three modes, one after another. Returns the failing cases in all.
uint32_t fault_run_all(const fault_run_t *r, fault_result_t *res);

// --- fingerprints ---------------------------------------------------------------------------

#define FSCK_FP_PATH_MAX 512

typedef struct {
    char path[FSCK_FP_PATH_MAX];
    bool dir;
    bool read_ok;           // FatFs read the whole file without an error
    uint64_t size;
    uint64_t hash;          // FNV-1a 64 of the bytes read
} fsck_fp_entry_t;

typedef struct {
    fsck_fp_entry_t *e;     // sorted by path
    size_t n, cap;
} fsck_fp_t;

uint64_t fsck_fnv1a64(uint64_t h, const void *p, size_t n);
#define FSCK_FNV1A64_INIT 0xCBF29CE484222325ull

// Mount `disk` read-only with host FatFs, walk every directory and file, and unmount. False if
// FatFs can't mount it or can't read a directory; a file whose read fails is still recorded,
// with read_ok false.
bool fsck_fp_take(fsck_disk_t *disk, fsck_fp_t *fp);
void fsck_fp_free(fsck_fp_t *fp);
// Differences between two fingerprints (a path in one only, or a different kind, size, read
// status or hash), skipping paths in `exclude` (exactly, or anything under one ending in '/').
// Each difference is printed to `out` (NULL: not printed). Returns how many.
size_t fsck_fp_diff(const fsck_fp_t *a, const fsck_fp_t *b, const char *const *exclude,
                    size_t n_exclude, FILE *out);
// §10.4's last check: host FatFs mounts `disk`, writes a new file (`path`, `size` bytes of
// fsck_disk_pattern()), reads it back, and walks the whole tree. The file stays on the disk.
// False if the write or the read-back fails or differs, or a directory can't be read (a file
// already damaged may still fail to read: compare fingerprints for those).
bool fsck_fp_write_probe(fsck_disk_t *disk, const char *path, uint32_t size);

#endif
