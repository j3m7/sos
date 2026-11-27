// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The exFAT parts of the card checker: boot region, entry sets, up-case table, and the cluster
// accounting a window at a time. fsck_exfat.h has the API and the rules decided here.

#include "files/fsck_exfat.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// Boot sector fields (ff.c's BPB_*Ex names).
#define BS_55AA             510
#define BPB_ZEROED          11      // 53 bytes that must be zero
#define BPB_ZEROED_LEN      53
#define BPB_TOT_SEC         72
#define BPB_FAT_OFS         80
#define BPB_FAT_SZ          84
#define BPB_DATA_OFS        88
#define BPB_NUM_CLUS        92
#define BPB_ROOT_CLUS       96
#define BPB_FS_VER          104
#define BPB_VOL_FLAG        106     // 2 bytes, left out of the checksum
#define BPB_BYTS_PER_SEC    108
#define BPB_SEC_PER_CLUS    109
#define BPB_NUM_FATS        110
#define BPB_PERC_IN_USE     112     // left out of the checksum

#define SECTOR              512u
#define SECTOR_SHIFT        9
#define MAX_EXFAT           0x7FFFFFFDu     // ff.c: FatFs's own limit on clusters
#define CLUSTER_SHIFT_MAX   15              // ff.c keeps the cluster size in a WORD
#define FAT_OFFSET_MIN      24              // the FAT comes after both boot regions
#define BOOT_SECTORS        12              // one boot region
#define UPCASE_BYTES_MAX    131072u         // a mapping for every UTF-16 code unit

// Entry types and fields (ff.c's ET_* and XDIR_*).
#define ET_BITMAP           0x81
#define ET_UPCASE           0x82
#define ET_VLABEL           0x83
#define ET_FILEDIR          0x85
#define ET_STREAM           0xC0
#define ET_FILENAME         0xC1
#define ET_IN_USE           0x80
#define ET_SECONDARY        0x40
#define ET_BENIGN           0x20

#define XDIR_NUM_SEC        1
#define XDIR_SET_SUM        2
#define XDIR_ATTR           4
#define XDIR_GEN_FLAGS      1
#define XDIR_NUM_NAME       3
#define XDIR_NAME_HASH      4
#define XDIR_VALID_SIZE     8
#define XDIR_FST_CLUS       20
#define XDIR_FILE_SIZE      24
#define XDIR_CASE_SUM       4
#define XDIR_NUM_LABEL      1

#define ENTRY               32u
#define NAME_PER_ENTRY      15u
#define SECONDARIES_MIN     2       // a stream entry and one name entry (ff.c's load_xdir())
#define SECONDARIES_MAX     18      // 19 entries in all
#define LABEL_MAX           11
#define AM_DIR              0x10
#define NO_FAT_CHAIN        0x02

// --- small helpers --------------------------------------------------------------------------

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p) {
    return le32(p) | (uint64_t)le32(p + 4) << 32;
}

// The checksums of exFAT: rotate right one bit, then add (16 and 32 bits wide).
static uint16_t sum16(uint16_t sum, uint8_t b) {
    return (uint16_t)(((sum & 1) ? 0x8000u : 0) + (sum >> 1) + b);
}

static uint32_t sum32(uint32_t sum, uint8_t b) {
    return ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + b;
}

// Bytes of the boot sector that change without the checksum changing.
static bool volatile_byte(size_t i) {
    return i == BPB_VOL_FLAG || i == BPB_VOL_FLAG + 1 || i == BPB_PERC_IN_USE;
}

// ceil(bytes / cluster_bytes), without overflowing for any 64-bit length.
static uint64_t clusters_for(uint64_t bytes, uint32_t cluster_bytes) {
    return bytes / cluster_bytes + (bytes % cluster_bytes != 0);
}

// --- boot region ----------------------------------------------------------------------------

bool fsck_exfat_is_vbr(const uint8_t sec[512]) {
    return le16(sec + BS_55AA) == 0xAA55 && !memcmp(sec, "\xEB\x76\x90" "EXFAT   ", 11);
}

const char *fsck_exfat_vbr(const uint8_t sec[512], uint64_t volume_lba, uint64_t device_sectors,
                           fsck_exfat_geom_t *g) {
    memset(g, 0, sizeof(*g));
    if (!fsck_exfat_is_vbr(sec))
        return "not an exFAT boot sector";
    // mount_volume()'s refusals, in its order.
    for (size_t i = BPB_ZEROED; i < BPB_ZEROED + BPB_ZEROED_LEN; i++)
        if (sec[i])
            return "bytes 11-63 of the boot sector not zero";
    if (le16(sec + BPB_FS_VER) != 0x100)
        return "exFAT version not 1.0";
    if (sec[BPB_BYTS_PER_SEC] != SECTOR_SHIFT)
        return "bytes per sector not 512";
    uint64_t vol_sectors = le64(sec + BPB_TOT_SEC);
    if (sec[BPB_NUM_FATS] != 1)
        return "number of FATs not 1";
    if (sec[BPB_SEC_PER_CLUS] > CLUSTER_SHIFT_MAX)
        return "cluster size over 32768 sectors";
    uint32_t csize = 1u << sec[BPB_SEC_PER_CLUS];
    uint32_t count = le32(sec + BPB_NUM_CLUS);
    if (count > MAX_EXFAT)
        return "too many clusters for exFAT";
    uint32_t heap = le32(sec + BPB_DATA_OFS), fat = le32(sec + BPB_FAT_OFS);
    uint32_t fat_len = le32(sec + BPB_FAT_SZ);
    if (vol_sectors < (uint64_t)heap + (uint64_t)count * csize)
        return "volume smaller than its cluster heap";
    // The design note's, beyond FatFs's: what the checker can't read past.
    if (!count)
        return "cluster count 0";
    if (volume_lba > device_sectors || vol_sectors > device_sectors - volume_lba)
        return "volume larger than the device";
    if (fat < FAT_OFFSET_MIN)
        return "FAT inside the boot regions";
    if ((uint64_t)fat_len * (SECTOR / 4) < (uint64_t)count + 2)
        return "FAT too small for the cluster count";
    if ((uint64_t)fat + fat_len > heap)
        return "FAT overlaps the cluster heap";
    uint32_t root = le32(sec + BPB_ROOT_CLUS);
    if (root < 2 || root > count + 1)
        return "root directory cluster out of range";
    g->volume_lba = volume_lba;
    g->volume_sectors = vol_sectors;
    g->fat_offset = fat;
    g->fat_sectors = fat_len;
    g->heap_offset = heap;
    g->clusters = count;
    g->root_cluster = root;
    g->cluster_sectors = csize;
    g->cluster_bytes = csize * SECTOR;
    g->volume_flags = le16(sec + BPB_VOL_FLAG);
    return NULL;
}

void fsck_exfat_boot_init(fsck_exfat_boot_t *b) {
    memset(b, 0, sizeof(*b));
}

// FNV-1a of a boot sector, without the bytes that may differ between main and backup.
static uint32_t boot_digest(const uint8_t *sec, bool first) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < SECTOR; i++)
        if (!first || !volatile_byte(i))
            h = (h ^ sec[i]) * 16777619u;
    return h;
}

void fsck_exfat_boot_sector(fsck_exfat_boot_t *b, const uint8_t sec[512]) {
    if (b->next >= 2 * BOOT_SECTORS)
        return;
    uint32_t idx = b->next++, r = idx % BOOT_SECTORS;
    if (r == 0)
        b->sum = 0;
    if (r < BOOT_SECTORS - 1) {
        for (size_t i = 0; i < SECTOR; i++)
            if (r || !volatile_byte(i))
                b->sum = sum32(b->sum, sec[i]);
    } else if (idx == BOOT_SECTORS - 1) {
        for (size_t i = 0; i < SECTOR; i += 4)
            if (le32(sec + i) != b->sum) {
                b->problems |= FSCK_EXFAT_B_CHECKSUM;
                break;
            }
    }
    uint32_t d = boot_digest(sec, r == 0);
    if (idx < BOOT_SECTORS) {
        b->digest[r] = d;
    } else if (d != b->digest[r] && !(b->problems & FSCK_EXFAT_B_BACKUP)) {
        b->problems |= FSCK_EXFAT_B_BACKUP;
        b->first_diff = idx;
    }
}

uint32_t fsck_exfat_boot_done(const fsck_exfat_boot_t *b) {
    return b->problems;
}

void fsck_exfat_boot_text(const fsck_exfat_boot_t *b, uint32_t bit, char *out, size_t n) {
    if (bit == FSCK_EXFAT_B_CHECKSUM)
        snprintf(out, n, "boot region checksum wrong (sector 11)");
    else if (bit == FSCK_EXFAT_B_BACKUP)
        snprintf(out, n, "backup boot region differs from the main one (sector %" PRIu32 ")",
                 b->first_diff);
    else
        snprintf(out, n, "boot region problem 0x%" PRIx32, bit);
}

// --- entry sets -----------------------------------------------------------------------------

uint16_t fsck_exfat_name_hash(const uint16_t *name, size_t len, fsck_exfat_upcase_fn upcase) {
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        uint16_t c = (uint16_t)upcase(name[i]);
        sum = sum16(sum, (uint8_t)c);
        sum = sum16(sum, (uint8_t)(c >> 8));
    }
    return sum;
}

void fsck_exfat_dir_begin(fsck_exfat_dir_t *d, const fsck_exfat_geom_t *geom,
                          fsck_exfat_upcase_fn upcase, bool root) {
    memset(d, 0, sizeof(*d));
    d->geom = geom;
    d->upcase = upcase;
    d->root = root;
}

static void sum_entry(fsck_exfat_dir_t *d, const uint8_t *e, bool primary) {
    for (size_t i = 0; i < ENTRY; i++)
        if (!primary || (i != XDIR_SET_SUM && i != XDIR_SET_SUM + 1))
            d->sum = sum16(d->sum, e[i]);
}

// The cluster fields of a set whose first cluster and length are read: range, count, run.
static void set_clusters(const fsck_exfat_dir_t *d, fsck_exfat_set_t *s) {
    uint32_t count = d->geom->clusters;
    if (!s->length) {
        if (s->first_cluster)
            s->problems |= FSCK_EXFAT_S_FIRST_CLUSTER;
        return;
    }
    uint64_t need = clusters_for(s->length, d->geom->cluster_bytes);
    bool in_range = s->first_cluster >= 2 && s->first_cluster <= count + 1;
    if (!in_range)
        s->problems |= FSCK_EXFAT_S_FIRST_CLUSTER;
    if (need > count) {
        s->problems |= FSCK_EXFAT_S_TOO_BIG;
        need = count;
    }
    s->clusters = (uint32_t)need;
    if (!s->no_fat_chain || !in_range)
        return;
    uint32_t room = count + 2 - s->first_cluster;
    if (need > room && !(s->problems & FSCK_EXFAT_S_TOO_BIG))
        s->problems |= FSCK_EXFAT_S_PAST_HEAP;
    s->run_clusters = need > room ? room : (uint32_t)need;
}

// File Name entries a name needs: at least one, as FatFs's load_xdir() reads at least one.
static uint32_t name_entries_for(uint32_t len) {
    return len ? (len + NAME_PER_ENTRY - 1) / NAME_PER_ENTRY : 1;
}

// The name's characters as FatFs's create_name() and get_fileinfo() take them.
static void check_name(fsck_exfat_set_t *s) {
    const uint16_t *nm = s->name;
    size_t len = s->name_len;
    if (!len) {
        s->problems |= FSCK_EXFAT_S_NAME_EMPTY;
        return;
    }
    for (size_t i = 0; i < len; i++) {
        uint16_t c = nm[i];
        bool bad;
        if (c >= 0xD800 && c <= 0xDBFF) {       // a high surrogate needs a low one next
            bad = i + 1 >= len || nm[i + 1] < 0xDC00 || nm[i + 1] > 0xDFFF;
            if (!bad)
                i++;
        } else if (c >= 0xDC00 && c <= 0xDFFF) {
            bad = true;
        } else {
            bad = c < 0x20 || (c < 0x80 && strchr("\"*/:<>?\\|\x7F", c));
        }
        if (bad) {
            s->problems |= FSCK_EXFAT_S_NAME_CHAR;
            s->bad_char = c;
            break;
        }
    }
    if (nm[0] == '.' && (len == 1 || (len == 2 && nm[1] == '.')))
        s->problems |= FSCK_EXFAT_S_NAME_DOT;
}

// The File entry set is finished: complete (every secondary read) or cut short.
static uint32_t finish_set(fsck_exfat_dir_t *d, bool complete) {
    fsck_exfat_set_t *s = &d->set;
    d->in_set = false;
    if (!complete)
        s->problems |= FSCK_EXFAT_S_SHORT;
    s->sum_found = d->sum;
    if (complete && s->sum_found != s->sum_stored)
        s->problems |= FSCK_EXFAT_S_CHECKSUM;
    if (s->usable) {
        uint32_t needed = name_entries_for(s->name_len);
        bool order = !(s->problems & (FSCK_EXFAT_S_NO_STREAM | FSCK_EXFAT_S_NOT_NAME));
        if (complete && order && !(s->problems & FSCK_EXFAT_S_COUNT) &&
            s->name_entries != needed)
            s->problems |= FSCK_EXFAT_S_NAME_ENTRIES;
        if (s->name_have == s->name_len) {
            check_name(s);
            if (!(s->problems & FSCK_EXFAT_S_NAME_ENTRIES)) {
                s->hash_found = fsck_exfat_name_hash(s->name, s->name_len, d->upcase);
                if (s->hash_found != s->hash_stored)
                    s->problems |= FSCK_EXFAT_S_HASH;
            }
        }
        if (s->valid_length > s->length)
            s->problems |= FSCK_EXFAT_S_VALID_LENGTH;
        set_clusters(d, s);
    }
    d->leftovers = (s->problems & (FSCK_EXFAT_S_COUNT | FSCK_EXFAT_S_NAME_ENTRIES)) != 0;
    return FSCK_EXFAT_SET;
}

// One secondary entry of the File entry set being read.
static void set_secondary(fsck_exfat_dir_t *d, const uint8_t *e) {
    fsck_exfat_set_t *s = &d->set;
    uint8_t pos = ++s->seen;
    sum_entry(d, e, false);
    if (pos == 1) {
        if (e[0] != ET_STREAM) {
            s->problems |= FSCK_EXFAT_S_NO_STREAM;
            s->bad_type = e[0];
            return;
        }
        s->usable = true;
        s->no_fat_chain = (e[XDIR_GEN_FLAGS] & NO_FAT_CHAIN) != 0;
        s->name_len = e[XDIR_NUM_NAME];
        s->hash_stored = le16(e + XDIR_NAME_HASH);
        s->valid_length = le64(e + XDIR_VALID_SIZE);
        s->first_cluster = le32(e + XDIR_FST_CLUS);
        s->length = le64(e + XDIR_FILE_SIZE);
        return;
    }
    if (e[0] != ET_FILENAME) {
        if (!(s->problems & (FSCK_EXFAT_S_NO_STREAM | FSCK_EXFAT_S_NOT_NAME))) {
            s->problems |= FSCK_EXFAT_S_NOT_NAME;
            s->bad_type = e[0];
            s->bad_pos = pos;
        }
        return;
    }
    s->name_entries++;
    if (!s->usable || s->problems & FSCK_EXFAT_S_NOT_NAME)
        return;
    for (size_t i = 0; i < NAME_PER_ENTRY && s->name_have < s->name_len; i++)
        s->name[s->name_have++] = le16(e + 2 + 2 * i);
}

static uint32_t start_set(fsck_exfat_dir_t *d, const uint8_t *e, uint32_t idx) {
    fsck_exfat_set_t *s = &d->set;
    memset(s, 0, sizeof(*s));
    s->kind = FSCK_EXFAT_FILE;
    s->index = idx;
    s->secondaries = e[XDIR_NUM_SEC];
    s->sum_stored = le16(e + XDIR_SET_SUM);
    s->attributes = le16(e + XDIR_ATTR);
    s->dir = (s->attributes & AM_DIR) != 0;
    if (s->secondaries < SECONDARIES_MIN || s->secondaries > SECONDARIES_MAX)
        s->problems |= FSCK_EXFAT_S_COUNT;
    d->sum = 0;
    sum_entry(d, e, true);
    d->in_set = true;
    return s->secondaries ? 0 : finish_set(d, true);
}

// One of the root's critical entries: the bitmap, the up-case table or the label.
static uint32_t root_entry(fsck_exfat_dir_t *d, const uint8_t *e, uint32_t idx) {
    fsck_exfat_set_t *s = &d->set;
    memset(s, 0, sizeof(*s));
    s->index = idx;
    uint32_t seen;
    if (e[0] == ET_VLABEL) {
        s->kind = FSCK_EXFAT_LABEL;
        seen = ++d->labels;
        if (e[XDIR_NUM_LABEL] > LABEL_MAX)
            s->problems |= FSCK_EXFAT_S_LABEL_LENGTH;
        s->length = e[XDIR_NUM_LABEL];
    } else {
        s->first_cluster = le32(e + XDIR_FST_CLUS);
        s->length = le64(e + XDIR_FILE_SIZE);
        if (e[0] == ET_BITMAP) {
            s->kind = FSCK_EXFAT_BITMAP;
            seen = ++d->bitmaps;
            if ((uint64_t)idx * ENTRY >= d->geom->cluster_bytes)
                s->problems |= FSCK_EXFAT_S_NOT_FIRST;
            if (s->length < (d->geom->clusters + 7u) / 8)
                s->problems |= FSCK_EXFAT_S_BITMAP_SIZE;
        } else {
            s->kind = FSCK_EXFAT_UPCASE;
            seen = ++d->upcases;
            s->table_checksum = le32(e + XDIR_CASE_SUM);
            if (!s->length || s->length > UPCASE_BYTES_MAX || s->length % 2)
                s->problems |= FSCK_EXFAT_S_UPCASE_SIZE;
        }
        set_clusters(d, s);
        s->usable = seen == 1;
    }
    if (seen > 1)
        s->problems |= FSCK_EXFAT_S_EXTRA;
    return FSCK_EXFAT_SET;
}

static uint32_t dir_problem(fsck_exfat_dir_t *d, uint32_t bit, const uint8_t *e, uint32_t idx) {
    d->problems |= bit;
    d->bad_type = e[0];
    d->bad_index = idx;
    return FSCK_EXFAT_DIR;
}

uint32_t fsck_exfat_entry(fsck_exfat_dir_t *d, const uint8_t e[32]) {
    d->problems = 0;
    if (d->ended)
        return FSCK_EXFAT_END;
    uint8_t type = e[0];
    bool secondary = (type & (ET_IN_USE | ET_SECONDARY)) == (ET_IN_USE | ET_SECONDARY);
    if (d->in_set) {
        if (!secondary)                 // a free or primary entry: the set ends early
            return finish_set(d, false) | FSCK_EXFAT_AGAIN;
        d->index++;
        set_secondary(d, e);
        return d->set.seen == d->set.secondaries ? finish_set(d, true) : 0;
    }
    uint32_t idx = d->index++;
    if (d->skip) {                      // a benign primary's secondaries
        if (secondary) {
            d->skip--;
            return 0;
        }
        d->skip = 0;
    }
    if (type == 0) {
        d->ended = true;
        return FSCK_EXFAT_END;
    }
    if (!(type & ET_IN_USE)) {           // deleted
        d->in_stray = false;
        return 0;
    }
    if (secondary) {
        if (type & ET_BENIGN)
            return 0;
        bool report = !d->in_stray && !d->leftovers;
        d->in_stray = true;
        return report ? dir_problem(d, FSCK_EXFAT_D_STRAY, e, idx) : 0;
    }
    d->in_stray = false;
    d->leftovers = false;
    if (type & ET_BENIGN) {
        d->skip = e[XDIR_NUM_SEC];
        return 0;
    }
    switch (type) {
    case ET_FILEDIR:
        return start_set(d, e, idx);
    case ET_BITMAP:
    case ET_UPCASE:
    case ET_VLABEL:
        return d->root ? root_entry(d, e, idx) : dir_problem(d, FSCK_EXFAT_D_ROOT_ONLY, e, idx);
    default:
        return dir_problem(d, FSCK_EXFAT_D_UNKNOWN, e, idx);
    }
}

uint32_t fsck_exfat_dir_end(fsck_exfat_dir_t *d) {
    d->problems = 0;
    uint32_t r = 0;
    if (d->in_set)
        r |= finish_set(d, false);
    d->ended = true;
    if (d->root && !d->bitmaps)
        d->problems |= FSCK_EXFAT_D_NO_BITMAP;
    if (d->root && !d->upcases)
        d->problems |= FSCK_EXFAT_D_NO_UPCASE;
    return r | (d->problems ? FSCK_EXFAT_DIR : 0);
}

const char *fsck_exfat_kind_name(fsck_exfat_kind_t k) {
    switch (k) {
    case FSCK_EXFAT_BITMAP:
        return "allocation bitmap";
    case FSCK_EXFAT_UPCASE:
        return "up-case table";
    case FSCK_EXFAT_LABEL:
        return "volume label";
    case FSCK_EXFAT_FILE:
        break;
    }
    return "file";
}

void fsck_exfat_set_text(const fsck_exfat_set_t *s, uint32_t bit, char *out, size_t n) {
    // The root's entries have no name, nor has a set whose name wasn't read: the line says
    // which entry it is about.
    int at = 0;
    if (s->kind != FSCK_EXFAT_FILE || !s->name_have)
        at = snprintf(out, n, "%s entry %" PRIu32 ": ", fsck_exfat_kind_name(s->kind), s->index);
    if (at < 0 || (size_t)at >= n)
        return;
    out += at;
    n -= (size_t)at;
    switch (bit) {
    case FSCK_EXFAT_S_SHORT:
        snprintf(out, n, "entry set ends after %u of %u secondary entries", s->seen,
                 s->secondaries);
        break;
    case FSCK_EXFAT_S_COUNT:
        snprintf(out, n, "secondary count %u (FatFs reads 2 to 18)", s->secondaries);
        break;
    case FSCK_EXFAT_S_NO_STREAM:
        snprintf(out, n, "entry type 0x%02X where the stream extension should be", s->bad_type);
        break;
    case FSCK_EXFAT_S_NOT_NAME:
        snprintf(out, n, "entry type 0x%02X where file name entry %u should be", s->bad_type,
                 s->bad_pos - 1u);
        break;
    case FSCK_EXFAT_S_CHECKSUM:
        snprintf(out, n, "set checksum 0x%04X, computed 0x%04X", s->sum_stored, s->sum_found);
        break;
    case FSCK_EXFAT_S_NAME_ENTRIES:
        snprintf(out, n, "secondary count %u doesn't fit a name of %u characters (needs %"
                 PRIu32 ")", s->secondaries, s->name_len,
                 1u + name_entries_for(s->name_len));
        break;
    case FSCK_EXFAT_S_HASH:
        snprintf(out, n, "name hash 0x%04X, computed 0x%04X", s->hash_stored, s->hash_found);
        break;
    case FSCK_EXFAT_S_NAME_CHAR:
        if (s->bad_char >= 0xD800 && s->bad_char <= 0xDFFF)
            snprintf(out, n, "name has a broken surrogate pair (U+%04X)", s->bad_char);
        else
            snprintf(out, n, "name has a character FatFs refuses (U+%04X)", s->bad_char);
        break;
    case FSCK_EXFAT_S_NAME_EMPTY:
        snprintf(out, n, "empty name");
        break;
    case FSCK_EXFAT_S_NAME_DOT:
        snprintf(out, n, "name is \".\" or \"..\"");
        break;
    case FSCK_EXFAT_S_VALID_LENGTH:
        snprintf(out, n, "valid data length %" PRIu64 " is larger than the data length %" PRIu64,
                 s->valid_length, s->length);
        break;
    case FSCK_EXFAT_S_FIRST_CLUSTER:
        if (!s->length)
            snprintf(out, n, "size 0 but first cluster %" PRIu32, s->first_cluster);
        else
            snprintf(out, n, "first cluster %" PRIu32 " out of range", s->first_cluster);
        break;
    case FSCK_EXFAT_S_TOO_BIG:
        snprintf(out, n, "size %" PRIu64 " needs more clusters than the volume has (%" PRIu32 ")",
                 s->length, s->clusters);
        break;
    case FSCK_EXFAT_S_PAST_HEAP:
        snprintf(out, n, "contiguous clusters %" PRIu32 "-%" PRIu64 " run past the last cluster (%"
                 PRIu32 ")", s->first_cluster, (uint64_t)s->first_cluster + s->clusters - 1,
                 s->first_cluster + s->run_clusters - 1);
        break;
    case FSCK_EXFAT_S_EXTRA:
        snprintf(out, n, "a second %s entry (FatFs uses the first)", fsck_exfat_kind_name(s->kind));
        break;
    case FSCK_EXFAT_S_NOT_FIRST:
        snprintf(out, n, "not in the root directory's first cluster (FatFs won't mount the "
                 "volume)");
        break;
    case FSCK_EXFAT_S_BITMAP_SIZE:
        snprintf(out, n, "%" PRIu64 " bytes, too small for the volume's clusters", s->length);
        break;
    case FSCK_EXFAT_S_UPCASE_SIZE:
        snprintf(out, n, "%" PRIu64 " bytes (2 to 131072, even)", s->length);
        break;
    case FSCK_EXFAT_S_LABEL_LENGTH:
        snprintf(out, n, "%" PRIu64 " characters (at most 11)", s->length);
        break;
    default:
        snprintf(out, n, "entry set problem 0x%" PRIx32, bit);
        break;
    }
}

void fsck_exfat_dir_text(const fsck_exfat_dir_t *d, uint32_t bit, char *out, size_t n) {
    switch (bit) {
    case FSCK_EXFAT_D_UNKNOWN:
        snprintf(out, n, "unknown critical entry type 0x%02X (entry %" PRIu32 ")", d->bad_type,
                 d->bad_index);
        break;
    case FSCK_EXFAT_D_ROOT_ONLY:
        snprintf(out, n, "entry type 0x%02X belongs in the root only (entry %" PRIu32 ")",
                 d->bad_type, d->bad_index);
        break;
    case FSCK_EXFAT_D_STRAY:
        snprintf(out, n, "secondary entry type 0x%02X outside an entry set (entry %" PRIu32 ")",
                 d->bad_type, d->bad_index);
        break;
    case FSCK_EXFAT_D_NO_BITMAP:
        snprintf(out, n, "no allocation bitmap entry");
        break;
    case FSCK_EXFAT_D_NO_UPCASE:
        snprintf(out, n, "no up-case table entry");
        break;
    default:
        snprintf(out, n, "directory problem 0x%" PRIx32, bit);
        break;
    }
}

// --- up-case table --------------------------------------------------------------------------

#define CODE_END 0x10000u

void fsck_exfat_upcase_init(fsck_exfat_upcase_t *u, uint32_t length, fsck_exfat_upcase_fn upcase) {
    memset(u, 0, sizeof(*u));
    u->length = length;
    u->upcase = upcase;
}

static void upcase_differs(fsck_exfat_upcase_t *u, uint32_t c) {
    if (!u->differ++)
        u->first_differ = c;
}

// Characters from u->code on, `n` of them, that the table maps to themselves.
static void upcase_identity(fsck_exfat_upcase_t *u, uint32_t n) {
    uint32_t end = n > CODE_END - u->code ? CODE_END : u->code + n;
    for (; u->code < end; u->code++)
        if (u->upcase(u->code) != u->code)
            upcase_differs(u, u->code);
}

static void upcase_unit(fsck_exfat_upcase_t *u, uint16_t w) {
    if (u->count_next) {
        u->count_next = false;
        upcase_identity(u, w);
    } else if (w == 0xFFFF) {
        u->count_next = true;
    } else if (u->code < CODE_END) {
        if (u->upcase(u->code) != w)
            upcase_differs(u, u->code);
        u->code++;
    }
}

void fsck_exfat_upcase_feed(fsck_exfat_upcase_t *u, const uint8_t *buf, size_t n) {
    for (size_t i = 0; i < n && u->fed < u->length; i++) {
        uint8_t b = buf[i];
        u->sum = sum32(u->sum, b);
        u->fed++;
        if (u->odd)
            upcase_unit(u, (uint16_t)(u->low | b << 8));
        else
            u->low = b;
        u->odd = !u->odd;
    }
}

uint32_t fsck_exfat_upcase_done(fsck_exfat_upcase_t *u, uint32_t table_checksum) {
    u->count_next = false;
    upcase_identity(u, CODE_END);
    uint32_t r = 0;
    if (u->sum != table_checksum)
        r |= FSCK_EXFAT_U_CHECKSUM;
    if (u->differ)
        r |= FSCK_EXFAT_U_DIFFERS;
    u->stored = table_checksum;
    return r;
}

void fsck_exfat_upcase_text(const fsck_exfat_upcase_t *u, uint32_t bit, char *out, size_t n) {
    if (bit == FSCK_EXFAT_U_CHECKSUM)
        snprintf(out, n, "up-case table checksum 0x%08" PRIX32 ", computed 0x%08" PRIX32,
                 u->stored, u->sum);
    else if (bit == FSCK_EXFAT_U_DIFFERS)
        snprintf(out, n, "the up-case table maps %" PRIu32 " characters unlike FatFs (the first "
                 "U+%04" PRIX32 ")", u->differ, u->first_differ);
    else
        snprintf(out, n, "up-case table problem 0x%" PRIx32, bit);
}

// --- cluster accounting ---------------------------------------------------------------------

bool fsck_run_add(fsck_run_t *r, uint32_t c, fsck_run_t *done) {
    if (r->count && (uint64_t)r->first + r->count == c) {
        r->count++;
        return false;
    }
    bool ended = r->count != 0;
    if (ended)
        *done = *r;
    r->first = c;
    r->count = 1;
    return ended;
}

bool fsck_run_end(fsck_run_t *r, fsck_run_t *done) {
    if (!r->count)
        return false;
    *done = *r;
    r->count = 0;
    return true;
}

uint32_t fsck_window_mark_run(uint8_t *win, uint32_t win_first, uint32_t win_bits,
                              uint32_t first, uint32_t count, uint32_t *cross) {
    uint64_t lo = first > win_first ? first : win_first;
    uint64_t hi = (uint64_t)first + count, win_hi = (uint64_t)win_first + win_bits;
    if (hi > win_hi)
        hi = win_hi;
    uint32_t again = 0;
    for (uint64_t c = lo; c < hi; c++) {
        uint32_t i = (uint32_t)(c - win_first);
        uint8_t m = (uint8_t)(1u << (i & 7));
        if (win[i >> 3] & m) {
            if (!again++)
                *cross = (uint32_t)c;
        }
        win[i >> 3] |= m;
    }
    return again;
}

void fsck_bitcmp_init(fsck_bitcmp_t *c) {
    memset(c, 0, sizeof(*c));
}

void fsck_bitcmp_slice(fsck_bitcmp_t *c, const uint8_t *reached, const uint8_t *bitmap,
                       uint32_t first, uint32_t bits) {
    c->reached = reached;
    c->bitmap = bitmap;
    c->first = first;
    c->bits = bits;
    c->pos = 0;
}

// An open run that doesn't go on at cluster `at` (`more`: whether it does there) ends.
static bool run_ends(fsck_run_t *r, uint32_t at, bool more, fsck_run_t *out) {
    if (!r->count || ((uint64_t)r->first + r->count == at && more))
        return false;
    return fsck_run_end(r, out);
}

int fsck_bitcmp_next(fsck_bitcmp_t *c, fsck_run_t *run) {
    while (c->pos < c->bits) {
        uint32_t pos = c->pos, at = c->first + pos;
        uint8_t used = c->bitmap[pos >> 3], got = c->reached[pos >> 3];
        if (!(pos & 7) && c->bits - pos >= 8 && used == got) {
            // A whole byte with nothing to report: every open run ends before it.
            if (run_ends(&c->lost, at, false, run))
                return FSCK_RUN_LOST;
            if (run_ends(&c->freed, at, false, run))
                return FSCK_RUN_FREE;
            c->used_total += (uint32_t)__builtin_popcount(used);
            c->pos += 8;
            continue;
        }
        uint8_t m = (uint8_t)(1u << (pos & 7));
        bool lost = (used & m) && !(got & m), freed = (got & m) && !(used & m);
        if (run_ends(&c->lost, at, lost, run))
            return FSCK_RUN_LOST;
        if (run_ends(&c->freed, at, freed, run))
            return FSCK_RUN_FREE;
        fsck_run_t unused;
        if (lost) {
            fsck_run_add(&c->lost, at, &unused);
            c->lost_total++;
        }
        if (freed) {
            fsck_run_add(&c->freed, at, &unused);
            c->free_total++;
        }
        if (used & m)
            c->used_total++;
        c->pos++;
    }
    return 0;
}

int fsck_bitcmp_finish(fsck_bitcmp_t *c, fsck_run_t *run) {
    if (fsck_run_end(&c->lost, run))
        return FSCK_RUN_LOST;
    if (fsck_run_end(&c->freed, run))
        return FSCK_RUN_FREE;
    return 0;
}
