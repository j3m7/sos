// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Seeded, deterministic damage to a FAT32 or exFAT image, for testing the card checker's repair
// (docs/design/sd-check.md §10.4, "Random damage"). The damage is written straight into the
// image: the boot sector, FAT, FSInfo and exFAT's allocation bitmap are parsed here, not by
// FatFs. FatFs is only used, once, to list the files and where their entries are
// (damage_catalog()); exFAT entry sets are found with tests/fuzz/exfat_damage.h.
//
// Each fault records what it did (kind, files, clusters, values), so a test can predict the
// problems a check reports and which files may no longer read the same. Faults are kinds in a
// table (damage_kind_t): damage_fat32_kinds for FAT32 images, damage_exfat_kinds for exFAT.
//
// Unless a kind's purpose is to break it, the FSInfo free count is kept true (as FatFs keeps it
// when it allocates or frees), every FAT copy is written alike, and on exFAT the bitmap holds
// exactly the clusters in use (a cluster is free when its bit is clear).

#ifndef SOS_FSCK_DAMAGE_H
#define SOS_FSCK_DAMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "fsck_disk.h"

#define DAMAGE_PATH_MAX     512
#define DAMAGE_FAULTS_MAX   4
#define DAMAGE_CLUSTERS_MAX 16
#define DAMAGE_EOC          0x0FFFFFFFu

// --- the volume -----------------------------------------------------------------------------

// A FAT32 volume's layout, from its boot sector.
typedef struct {
    uint64_t vol;           // first sector of the volume
    uint64_t fatbase;       // first sector of FAT copy 0
    uint64_t database;      // first sector of cluster 2
    uint32_t spc;           // sectors per cluster
    uint32_t nfats;
    uint32_t fatsz;         // sectors per FAT copy
    uint32_t count;         // clusters: 2 … count + 1
    uint32_t root;          // root directory's first cluster
    uint32_t fsinfo;        // FSInfo sector, from the volume start (0: none)
} damage_fat32_t;

// Find volume 0 (a boot sector at 0, or the first MBR partition's) and read its BPB. False if
// it isn't FAT32.
bool damage_fat32_parse(const fsck_disk_t *d, damage_fat32_t *g);
// FAT entry `c` of copy `copy` (upper 4 bits dropped).
uint32_t damage_fat_get(const fsck_disk_t *d, const damage_fat32_t *g, uint32_t copy, uint32_t c);
// Set entry `c` (upper 4 bits kept) in copy `copy`, or in every copy when `copy` is
// DAMAGE_ALL_COPIES.
#define DAMAGE_ALL_COPIES UINT32_MAX
void damage_fat_set(fsck_disk_t *d, const damage_fat32_t *g, uint32_t copy, uint32_t c,
                    uint32_t v);
// Follow a chain from `first` through copy 0, storing up to `max` clusters in `out` (NULL: none).
// Returns its length, counting only clusters in range; *ended is true when it ended in an
// end-of-chain mark (not a free or bad entry, an out-of-range number, or a loop).
uint32_t damage_chain(const fsck_disk_t *d, const damage_fat32_t *g, uint32_t first,
                      uint32_t *out, uint32_t max, bool *ended);
// FSInfo's free count, or UINT32_MAX when its signatures are wrong.
uint32_t damage_fsinfo_free(const fsck_disk_t *d, const damage_fat32_t *g);
// Free entries in FAT copy 0, counted.
uint32_t damage_count_free(const fsck_disk_t *d, const damage_fat32_t *g);

// An exFAT volume's layout, from its boot sector and the root's bitmap entry.
typedef struct {
    uint64_t vol;           // first sector of the volume
    uint64_t fatbase;       // first sector of the FAT
    uint64_t heap;          // first sector of cluster 2
    uint64_t bitmap;        // first sector of the allocation bitmap (read contiguously)
    uint32_t spc;           // sectors per cluster
    uint32_t count;         // clusters: 2 … count + 1
    uint32_t root;          // root directory's first cluster
} damage_exfat_t;

#define DAMAGE_EXFAT_EOC    0xFFFFFFFFu

// Find volume 0 (a boot sector at 0, or the first MBR partition's), read its boot sector and
// find the bitmap in the root's first cluster. False if it isn't exFAT.
bool damage_exfat_parse(const fsck_disk_t *d, damage_exfat_t *g);
// FAT entry `c` (all 32 bits), and setting it.
uint32_t damage_xfat_get(const fsck_disk_t *d, const damage_exfat_t *g, uint32_t c);
void damage_xfat_set(fsck_disk_t *d, const damage_exfat_t *g, uint32_t c, uint32_t v);
// Cluster `c`'s bit in the allocation bitmap, and setting or clearing it.
bool damage_bit_get(const fsck_disk_t *d, const damage_exfat_t *g, uint32_t c);
void damage_bit_set(fsck_disk_t *d, const damage_exfat_t *g, uint32_t c, bool set);

// --- the catalogue --------------------------------------------------------------------------

typedef struct {
    char path[DAMAGE_PATH_MAX];
    bool dir;
    uint32_t clust;         // first cluster (0: none)
    uint64_t size;
    uint64_t dir_lba;       // FAT32: a file's short entry: sector and byte offset
    uint32_t dir_ofs;
    bool contig;            // exFAT: NoFatChain (its clusters follow each other, no FAT chain)
} damage_entry_t;

typedef struct {
    damage_entry_t *e;      // in fsck_disk_walk()'s order (breadth first)
    size_t n;
} damage_catalog_t;

// List every file and directory of the (unmounted) image with host FatFs.
bool damage_catalog(fsck_disk_t *d, damage_catalog_t *cat);
void damage_catalog_free(damage_catalog_t *cat);
// The entry for `path`, or -1.
int damage_catalog_find(const damage_catalog_t *cat, const char *path);

// --- faults ---------------------------------------------------------------------------------

typedef struct damage_kind damage_kind_t;

// What one fault did.
typedef struct {
    const damage_kind_t *kind;
    int file[2];            // catalogue entries involved (-1: none)
    bool broken[2];         // that file may no longer read the same bytes
    uint32_t clusters[DAMAGE_CLUSTERS_MAX];   // see each kind
    uint32_t n_clusters;
    uint32_t value[2];      // see each kind
    char what[200];         // one line, for failure reports
} damage_fault_t;

typedef struct {
    uint64_t seed;
    unsigned n;
    damage_fault_t f[DAMAGE_FAULTS_MAX];
} damage_t;

// The state a kind works on.
typedef struct {
    fsck_disk_t *disk;
    const damage_catalog_t *cat;
    bool exfat;             // which of the two layouts is valid
    damage_fat32_t fat;
    damage_exfat_t ex;
    uint64_t rng;
    bool *used;             // catalogue entries a fault already involves
    uint32_t reserved[1024];  // clusters faults touched or freed: never allocated again
    size_t n_reserved;
    bool fsinfo_stale;
} damage_ctx_t;

struct damage_kind {
    const char *name;
    bool repairable;        // §10.1 repairs it (else it's only reported)
    // Apply one fault, or return false having changed nothing (not applicable here).
    bool (*apply)(damage_ctx_t *c, damage_fault_t *out);
};

// The FAT32 kinds, in this order. clusters[] and value[] per kind:
enum {
    // repairable
    DAMAGE_LOST_CHAIN,      // clusters: the lost run (1-4), linked and ended
    DAMAGE_OVERLONG,        // file[0]; clusters: its last, then those added; value: needed, added
    DAMAGE_STALE_FSINFO,    // value: true free count, the one written
    DAMAGE_FAT_COPIES,      // clusters[0]: a free cluster marked end-of-chain in copy 1 only
    // only reported
    DAMAGE_CROSS_LINK,      // file[0] (broken) now runs into file[1] (intact); clusters: A's
                            // redirected cluster, B's cluster it now points to, then A's old
                            // tail, freed; value: A's new chain length, A's needed
    DAMAGE_BROKEN_CHAIN,    // file[0] (broken); clusters[0]: its cluster now free, then the
                            // clusters after it, now lost; value[0]: how many are lost
    DAMAGE_SIZE_LARGER,     // file[0] (broken); value: old size, new size (> the chain holds)
    DAMAGE_OVERLONG_XLINK,  // §10.4: file[0]'s last cluster now points into file[1], later in
                            // the walk; both intact; clusters: A's last, B's cluster;
                            // value: A's needed, the extra clusters A's chain now has
    DAMAGE_FAT32_KINDS
};
extern const damage_kind_t damage_fat32_kinds[DAMAGE_FAT32_KINDS];

// The exFAT kinds, in this order. "FAT-chained" files are those without NoFatChain.
enum {
    // repairable
    DAMAGE_X_LOST_RUN,      // clusters: 1-4 free clusters whose bits were set (no FAT change)
    DAMAGE_X_REACHED_FREE,  // file[0] (intact); clusters: 1-3 of its clusters, bits cleared
    DAMAGE_X_OVERLONG,      // file[0], FAT-chained; clusters: its last, then those added (bits
                            // set, linked, ended 0xFFFFFFFF); value: needed, added
    DAMAGE_X_PAST_RUN,      // file[0], NoFatChain; clusters: 1-3 right after its run, bits set
    // only reported
    DAMAGE_X_CROSS_LINK,    // as DAMAGE_CROSS_LINK, both FAT-chained; A's old tail's bits cleared
    DAMAGE_X_BROKEN_CHAIN,  // file[0] (broken), FAT-chained; clusters[0]: its cluster whose FAT
                            // entry is now 0 (bit kept), then those after it; value[0]: the
                            // clusters now lost, that one included. Repair frees nothing then
    DAMAGE_X_SIZE_LARGER,   // file[0] (broken), FAT-chained; value: old size, new DataLength
    DAMAGE_X_OVERLONG_XLINK,  // as DAMAGE_OVERLONG_XLINK, both FAT-chained
    DAMAGE_EXFAT_KINDS
};
extern const damage_kind_t damage_exfat_kinds[DAMAGE_EXFAT_KINDS];

// Clusters needed for `size` bytes.
uint32_t damage_needed(const damage_fat32_t *g, uint64_t size);

// Apply one fault of `kind`, chosen with `seed`. False (nothing changed) if it doesn't apply.
bool damage_apply(fsck_disk_t *d, const damage_catalog_t *cat, const damage_kind_t *kind,
                  uint64_t seed, damage_fault_t *out);
// One to four faults from `kinds`, the first of a repairable kind, chosen with `seed`.
// The same image, catalogue and seed give the same damage. False if none applied.
bool damage_random(fsck_disk_t *d, const damage_catalog_t *cat, const damage_kind_t *kinds,
                   size_t n_kinds, uint64_t seed, damage_t *out);
// The paths of the files `dm` broke (for fsck_fp_diff()'s exclude list); returns how many.
size_t damage_broken_paths(const damage_t *dm, const damage_catalog_t *cat, const char **out,
                           size_t max);
// One line per fault, with the seed.
void damage_print(const damage_t *dm, FILE *out);

#endif
