// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Disk images for the card checker's host tests (docs/design/sd-check.md §8, §10.4): a sparse
// RAM disk, FatFs's diskio over it (host-compiled ff.c and ffunicode.c see it as drive 0), and
// an image builder that formats with `sd format`'s parameters and fills a tree of files.
//
// The disk keeps a snapshot: fsck_disk_snapshot() marks the current contents, and
// fsck_disk_restore() puts them back by undoing only the sectors written since (an undo journal,
// not a copy), so a test can rewind an image hundreds of times cheaply.
//
// This file defines FatFs's disk_*(), get_fattime(), ff_memalloc() and ff_memfree(): a test
// program links it instead of defining its own.

#ifndef SOS_FSCK_DISK_H
#define SOS_FSCK_DISK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ff.h"

#define FSCK_DISK_ALIGN 8192u   // f_mkfs's align, as `sd format` passes it (FORMAT_ALIGN_SECTORS)

typedef struct fsck_disk fsck_disk_t;

// A disk of `sectors` 512-byte sectors, all zero. Sectors cost memory only once written with
// something other than zeros.
fsck_disk_t *fsck_disk_new(uint64_t sectors);
void fsck_disk_free(fsck_disk_t *d);
// A full, independent copy (no snapshot).
fsck_disk_t *fsck_disk_copy(const fsck_disk_t *d);
uint64_t fsck_disk_sectors(const fsck_disk_t *d);

// One sector. Out of range: get reads zeros, put does nothing.
void fsck_disk_get(const fsck_disk_t *d, uint64_t lba, uint8_t *buf);
void fsck_disk_put(fsck_disk_t *d, uint64_t lba, const uint8_t *buf);

// fsck_io_t-shaped callbacks (ctx: the fsck_disk_t). False when the range is past the end.
bool fsck_disk_read(void *ctx, uint64_t lba, uint32_t count, uint8_t *buf);
bool fsck_disk_write(void *ctx, uint64_t lba, uint32_t count, const uint8_t *buf);

// Same contents, sector by sector (sizes too).
bool fsck_disk_equal(const fsck_disk_t *a, const fsck_disk_t *b);

// Snapshot: from now on every first write to a sector saves its old contents. restore() puts
// them back and keeps the same snapshot, so it can be called again and again. snapshot() again
// moves the mark to the current contents.
void fsck_disk_snapshot(fsck_disk_t *d);
void fsck_disk_restore(fsck_disk_t *d);
// Sectors whose contents differ from the snapshot's, in the order they were first written:
// up to `max` LBAs into `out`; returns how many differ in all.
size_t fsck_disk_changed(const fsck_disk_t *d, uint64_t *out, size_t max);

// FatFs: drive "0:" is `d` (NULL: no disk). mount() attaches and mounts (false if FatFs won't);
// unmount() unmounts and detaches. fsck_disk_fs() is the FATFS while mounted.
void fsck_disk_attach(fsck_disk_t *d);
bool fsck_disk_mount(fsck_disk_t *d);
void fsck_disk_unmount(void);
FATFS *fsck_disk_fs(void);

// --- the builder ----------------------------------------------------------------------------

// f_mkfs on a new disk with `sd format`'s MKFS_PARM (on_format_confirm() in src/files/sdcard.c:
// align 8192, a partition table, n_fat 1) except where given: `fmt` FM_FAT32 or FM_EXFAT,
// `n_fat` 1 or 2, `au` the cluster size in bytes (0: f_mkfs's choice). NULL if f_mkfs fails.
// The disk is left unmounted.
fsck_disk_t *fsck_disk_format(uint64_t sectors, BYTE fmt, BYTE n_fat, DWORD au);

// The images the fault tests use: 128 MB, FAT32, 1 KB clusters (about 130000, above FAT32's
// 65526 minimum), MBR; one FAT as `sd format` makes it, or two as a PC might.
#define FSCK_DISK_SMALL_SECTORS (1u << 18)
fsck_disk_t *fsck_disk_fat32(BYTE n_fat);

// Write a file of `size` bytes whose contents depend on `path` and `salt`
// (fsck_disk_pattern()), creating or replacing it. The disk must be mounted.
bool fsck_disk_put_file(const char *path, uint32_t size, uint32_t salt);
// The byte at `pos` of such a file.
uint8_t fsck_disk_pattern(const char *path, uint32_t salt, uint32_t pos);

// A populated tree on the mounted disk: about 40 files in nested directories, long and
// Unicode names, empty files and directories, files of one cluster and of many, and a
// directory of several clusters. False if FatFs fails.
bool fsck_disk_populate(void);

// Every directory and file of the mounted disk, breadth first, without recursion. For a file,
// `fp` is the file opened for reading (the callback may read it; it's closed afterwards); for a
// directory it is NULL. `path` starts with "/". The callback returns false to stop the walk.
// Returns false if FatFs fails to read a directory or open a file (or the callback stopped).
typedef bool (*fsck_disk_walk_fn)(const char *path, const FILINFO *info, FIL *fp, void *ctx);
bool fsck_disk_walk(fsck_disk_walk_fn fn, void *ctx);

#endif
