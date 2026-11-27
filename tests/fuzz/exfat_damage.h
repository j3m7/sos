// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Damage for exFAT images: each exd_*() function breaks one thing in a volume FatFs wrote, as
// a power cut, a bad writer or a bad card might, so that a checker test can expect the exact
// problem. Used by tests/test_fsck.c, which runs each case through the whole checker, and by
// tests/test_fsck_exfat.c to find entry sets (design note §8, §10.4).
//
// The image is reached through a callback that returns a sector's 512 bytes, writable (a
// sector never written may be created as zeros), so any RAM disk can serve. Entry sets are
// found with fsck_exfat.c's own parser. Damage that changes an entry set fixes its SetChecksum
// afterwards, unless the damage is the checksum itself, so that each case breaks one thing.

#ifndef SOS_EXFAT_DAMAGE_H
#define SOS_EXFAT_DAMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "files/fsck_exfat.h"

typedef struct {
    uint8_t *(*sector)(void *ctx, uint64_t lba);    // the sector's bytes, writable
    void *ctx;
    fsck_exfat_geom_t g;
} exd_t;

// A directory: where its entries are.
typedef struct {
    uint32_t first;             // first cluster
    bool contiguous;            // NoFatChain: no FAT to follow
} exd_dir_t;

// An entry set found by exd_find(): its directory, where it starts, and what it says.
typedef struct {
    exd_dir_t dir;
    uint32_t index;             // the primary entry's index in the directory
    fsck_exfat_set_t set;
} exd_loc_t;

static inline uint32_t exd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline void exd_put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void exd_put32(uint8_t *p, uint32_t v) {
    exd_put16(p, (uint16_t)v);
    exd_put16(p + 2, (uint16_t)(v >> 16));
}

static inline void exd_put64(uint8_t *p, uint64_t v) {
    exd_put32(p, (uint32_t)v);
    exd_put32(p + 4, (uint32_t)(v >> 32));
}

static inline uint64_t exd_le64(const uint8_t *p) {
    return exd_le32(p) | (uint64_t)exd_le32(p + 4) << 32;
}

// Simple ASCII case folding: enough to find a set (the hash isn't what finds it here).
static inline uint32_t exd_upcase(uint32_t c) {
    return c >= 'a' && c <= 'z' ? c - 32 : c;
}

// Open the exFAT volume whose boot sector is at `volume_lba`. False if it isn't one.
static inline bool exd_open(exd_t *x, uint8_t *(*sector)(void *, uint64_t), void *ctx,
                            uint64_t volume_lba, uint64_t device_sectors) {
    x->sector = sector;
    x->ctx = ctx;
    return fsck_exfat_vbr(sector(ctx, volume_lba), volume_lba, device_sectors, &x->g) == NULL;
}

static inline uint8_t *exd_vol(const exd_t *x, uint64_t sector) {
    return x->sector(x->ctx, x->g.volume_lba + sector);
}

static inline uint32_t exd_fat(const exd_t *x, uint32_t c) {
    return exd_le32(exd_vol(x, x->g.fat_offset + c / 128) + c % 128 * 4);
}

static inline void exd_set_fat(const exd_t *x, uint32_t c, uint32_t v) {
    exd_put32(exd_vol(x, x->g.fat_offset + c / 128) + c % 128 * 4, v);
}

static inline uint64_t exd_cluster_sector(const exd_t *x, uint32_t c) {
    return x->g.heap_offset + (uint64_t)(c - 2) * x->g.cluster_sectors;
}

// The 32 bytes of entry `index` of directory `d`; NULL past its chain.
static inline uint8_t *exd_entry(const exd_t *x, exd_dir_t d, uint32_t index) {
    uint32_t per_cluster = x->g.cluster_bytes / 32, c = d.first;
    for (uint32_t n = index / per_cluster; n; n--) {
        c = d.contiguous ? c + 1 : exd_fat(x, c);
        if (c < 2 || c > x->g.clusters + 1)
            return NULL;
    }
    uint32_t in = index % per_cluster;
    return exd_vol(x, exd_cluster_sector(x, c) + in / 16) + in % 16 * 32;
}

// The allocation bitmap's bit for cluster `c` (the root's first bitmap entry says where).
static inline uint8_t *exd_bitmap_byte(const exd_t *x, uint32_t c, uint8_t *mask) {
    exd_dir_t root = {x->g.root_cluster, false};
    uint32_t first = 0;
    for (uint32_t i = 0; i < x->g.cluster_bytes / 32; i++) {
        const uint8_t *e = exd_entry(x, root, i);
        if (e[0] == 0x81) {
            first = exd_le32(e + 20);
            break;
        }
    }
    uint32_t bit = c - 2;
    *mask = (uint8_t)(1u << (bit & 7));
    return exd_vol(x, exd_cluster_sector(x, first) + bit / 8 / 512) + bit / 8 % 512;
}

// UTF-8 `s` up to `end` (or its NUL) as UTF-16; the number of code units.
static inline size_t exd_utf16(const char *s, const char *end, uint16_t *out, size_t max) {
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && (const char *)p < end && n < max) {
        uint32_t c = *p++;
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (more)
            c &= 0x3Fu >> more;
        while (more-- > 0 && *p)
            c = c << 6 | (*p++ & 0x3Fu);
        if (c >= 0x10000) {
            out[n++] = (uint16_t)(0xD800 | (c - 0x10000) >> 10);
            if (n < max)
                out[n++] = (uint16_t)(0xDC00 | (c & 0x3FF));
        } else {
            out[n++] = (uint16_t)c;
        }
    }
    return n;
}

// Find the set named `name` (exact UTF-16 match) in directory `d`.
static inline bool exd_find_in(const exd_t *x, exd_dir_t d, const uint16_t *name, size_t len,
                               exd_loc_t *loc) {
    fsck_exfat_dir_t rd;
    fsck_exfat_dir_begin(&rd, &x->g, exd_upcase, d.first == x->g.root_cluster);
    for (uint32_t i = 0;; i++) {
        const uint8_t *e = exd_entry(x, d, i);
        if (!e)
            return false;
        uint32_t r;
        do {
            r = fsck_exfat_entry(&rd, e);
            const fsck_exfat_set_t *s = &rd.set;
            if ((r & FSCK_EXFAT_SET) && s->kind == FSCK_EXFAT_FILE && s->usable &&
                s->name_have == len && !memcmp(s->name, name, len * 2)) {
                loc->dir = d;
                loc->index = s->index;
                loc->set = *s;
                return true;
            }
        } while (r & FSCK_EXFAT_AGAIN);
        if (r & FSCK_EXFAT_END)
            return false;
    }
}

// Find the set at `path` ("dir/sub/file", from the root; UTF-8).
static inline bool exd_find(const exd_t *x, const char *path, exd_loc_t *loc) {
    exd_dir_t d = {x->g.root_cluster, false};
    for (;;) {
        const char *slash = strchr(path, '/');
        const char *end = slash ? slash : path + strlen(path);
        uint16_t name[FSCK_EXFAT_NAME_MAX];
        size_t len = exd_utf16(path, end, name, FSCK_EXFAT_NAME_MAX);
        if (!exd_find_in(x, d, name, len, loc))
            return false;
        if (!slash)
            return true;
        d.first = loc->set.first_cluster;
        d.contiguous = loc->set.no_fat_chain;
        path = slash + 1;
    }
}

// The index of the first entry of type `type` in directory `d` (0x00: the end entry).
static inline bool exd_find_type(const exd_t *x, exd_dir_t d, uint8_t type, uint32_t *index) {
    for (uint32_t i = 0;; i++) {
        const uint8_t *e = exd_entry(x, d, i);
        if (!e)
            return false;
        if (e[0] == type) {
            *index = i;
            return true;
        }
        if (!e[0])
            return false;
    }
}

// Entry `k` of the set at `loc` (0: the File entry).
static inline uint8_t *exd_set_entry(const exd_t *x, const exd_loc_t *loc, uint32_t k) {
    return exd_entry(x, loc->dir, loc->index + k);
}

// Recompute the set's SetChecksum over SecondaryCount + 1 entries, as FatFs's xdir_sum().
static inline void exd_resum(const exd_t *x, const exd_loc_t *loc) {
    uint8_t *p = exd_set_entry(x, loc, 0);
    uint32_t n = p[1] + 1u;
    uint16_t sum = 0;
    for (uint32_t k = 0; k < n; k++) {
        const uint8_t *e = exd_set_entry(x, loc, k);
        for (uint32_t i = 0; i < 32; i++)
            if (k || (i != 2 && i != 3))
                sum = (uint16_t)(((sum & 1) ? 0x8000u : 0) + (sum >> 1) + e[i]);
    }
    exd_put16(p + 2, sum);
}

// --- the damage -----------------------------------------------------------------------------

// SetChecksum off by one.
static inline void exd_bad_set_checksum(const exd_t *x, const exd_loc_t *loc) {
    uint8_t *p = exd_set_entry(x, loc, 0);
    exd_put16(p + 2, (uint16_t)(p[2] + (p[3] << 8) + 1));
}

// NameHash off by one.
static inline void exd_bad_name_hash(const exd_t *x, const exd_loc_t *loc) {
    uint8_t *s = exd_set_entry(x, loc, 1);
    exd_put16(s + 4, (uint16_t)(s[4] + (s[5] << 8) + 1));
    exd_resum(x, loc);
}

// SecondaryCount changed by `delta`; the checksum is recomputed over the new count.
static inline void exd_secondary_count(const exd_t *x, const exd_loc_t *loc, int delta) {
    uint8_t *p = exd_set_entry(x, loc, 0);
    p[1] = (uint8_t)(p[1] + delta);
    exd_resum(x, loc);
}

// NameLength set to `len`.
static inline void exd_name_length(const exd_t *x, const exd_loc_t *loc, uint8_t len) {
    exd_set_entry(x, loc, 1)[3] = len;
    exd_resum(x, loc);
}

// ValidDataLength one more than DataLength.
static inline void exd_valid_over(const exd_t *x, const exd_loc_t *loc) {
    uint8_t *s = exd_set_entry(x, loc, 1);
    exd_put64(s + 8, exd_le64(s + 24) + 1);
    exd_resum(x, loc);
}

// A NoFatChain file's DataLength (and ValidDataLength) made to reach `extra` clusters past the
// heap's last cluster.
static inline void exd_run_past_heap(const exd_t *x, const exd_loc_t *loc, uint32_t extra) {
    uint8_t *s = exd_set_entry(x, loc, 1);
    uint64_t n = (uint64_t)x->g.clusters + 2 - exd_le32(s + 20) + extra;
    exd_put64(s + 24, n * x->g.cluster_bytes);
    exd_put64(s + 8, n * x->g.cluster_bytes);
    exd_resum(x, loc);
}

// The root's Allocation Bitmap entry turned into a deleted one (no bitmap entry left).
static inline bool exd_no_bitmap(const exd_t *x) {
    exd_dir_t root = {x->g.root_cluster, false};
    uint32_t i;
    if (!exd_find_type(x, root, 0x81, &i))
        return false;
    exd_entry(x, root, i)[0] = 0x01;
    return true;
}

// A copy of the root's Allocation Bitmap entry written over its end entry (two bitmaps).
static inline bool exd_second_bitmap(const exd_t *x, uint32_t *index) {
    exd_dir_t root = {x->g.root_cluster, false};
    uint32_t i;
    if (!exd_find_type(x, root, 0x81, &i) || !exd_find_type(x, root, 0x00, index))
        return false;
    memcpy(exd_entry(x, root, *index), exd_entry(x, root, i), 32);
    return true;
}

// The Up-case Table entry's TableChecksum off by one.
static inline bool exd_bad_upcase_checksum(const exd_t *x) {
    exd_dir_t root = {x->g.root_cluster, false};
    uint32_t i;
    if (!exd_find_type(x, root, 0x82, &i))
        return false;
    uint8_t *e = exd_entry(x, root, i);
    exd_put32(e + 4, exd_le32(e + 4) + 1);
    return true;
}

// A byte of the boot code changed in both boot regions: the boot checksum is wrong, the
// backup still the same as the main region.
static inline void exd_bad_boot_checksum(const exd_t *x) {
    exd_vol(x, 0)[200] ^= 0x5A;
    exd_vol(x, 12)[200] ^= 0x5A;
}

// A byte of backup sector `sector` (12-23) changed.
static inline void exd_backup_differs(const exd_t *x, uint32_t sector) {
    exd_vol(x, sector)[100] ^= 0xA5;
}

// VolumeFlags' dirty bit set (left out of the checksum, as a driver sets it).
static inline void exd_dirty(const exd_t *x) {
    exd_vol(x, 0)[106] |= 0x02;
}

// Cluster `c`'s bit in the allocation bitmap cleared or set.
static inline void exd_bitmap_bit(const exd_t *x, uint32_t c, bool set) {
    uint8_t m, *b = exd_bitmap_byte(x, c, &m);
    *b = (uint8_t)(set ? *b | m : *b & ~m);
}

// An unknown critical primary entry (type 0x84) written over directory `d`'s end entry.
static inline bool exd_unknown_critical(const exd_t *x, exd_dir_t d, uint32_t *index) {
    if (!exd_find_type(x, d, 0x00, index))
        return false;
    uint8_t *e = exd_entry(x, d, *index);
    memset(e, 0, 32);
    e[0] = 0x84;
    return true;
}

#endif
