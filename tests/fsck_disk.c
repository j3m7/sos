// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The sparse RAM disk, its undo journal, FatFs's diskio over it, and the image builder
// (fsck_disk.h).

#include "fsck_disk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diskio.h"

#define SECTOR 512u

// --- the sparse disk ------------------------------------------------------------------------

// Written sectors in an open-addressed hash table. `gen` is the snapshot generation in which
// the sector's old contents were last saved to the journal.
typedef struct {
    uint64_t lba;
    uint8_t *data;
    uint32_t gen;
} slot_t;

// One journal entry: a sector's contents at the snapshot, saved before its first write.
typedef struct {
    uint64_t lba;
    uint8_t data[SECTOR];
} undo_t;

struct fsck_disk {
    uint64_t sectors;
    size_t cap, used;
    slot_t *slot;
    bool snap;              // a snapshot is kept
    uint32_t gen;           // current snapshot generation (never 0 while snap)
    undo_t *undo;
    size_t undo_len, undo_cap;
};

static void *xcalloc(size_t n, size_t size) {
    void *p = calloc(n, size);
    if (!p) {
        fprintf(stderr, "fsck_disk: out of memory\n");
        abort();
    }
    return p;
}

fsck_disk_t *fsck_disk_new(uint64_t sectors) {
    fsck_disk_t *d = xcalloc(1, sizeof(*d));
    d->sectors = sectors;
    d->cap = 1024;
    d->slot = xcalloc(d->cap, sizeof(slot_t));
    return d;
}

void fsck_disk_free(fsck_disk_t *d) {
    if (!d)
        return;
    for (size_t i = 0; i < d->cap; i++)
        free(d->slot[i].data);
    free(d->slot);
    free(d->undo);
    free(d);
}

uint64_t fsck_disk_sectors(const fsck_disk_t *d) {
    return d->sectors;
}

static size_t slot_hash(const fsck_disk_t *d, uint64_t lba) {
    return (size_t)((lba * 0x9E3779B97F4A7C15ull) >> 24) & (d->cap - 1);
}

static slot_t *slot_find(const fsck_disk_t *d, uint64_t lba) {
    size_t i = slot_hash(d, lba);
    while (d->slot[i].data) {
        if (d->slot[i].lba == lba)
            return &d->slot[i];
        i = (i + 1) & (d->cap - 1);
    }
    return NULL;
}

static void slot_grow(fsck_disk_t *d) {
    slot_t *old = d->slot;
    size_t n = d->cap;
    d->cap *= 2;
    d->slot = xcalloc(d->cap, sizeof(slot_t));
    for (size_t i = 0; i < n; i++) {
        if (!old[i].data)
            continue;
        size_t j = slot_hash(d, old[i].lba);
        while (d->slot[j].data)
            j = (j + 1) & (d->cap - 1);
        d->slot[j] = old[i];
    }
    free(old);
}

static slot_t *slot_add(fsck_disk_t *d, uint64_t lba) {
    if (2 * (d->used + 1) > d->cap)
        slot_grow(d);
    size_t i = slot_hash(d, lba);
    while (d->slot[i].data)
        i = (i + 1) & (d->cap - 1);
    d->slot[i].lba = lba;
    d->slot[i].data = xcalloc(1, SECTOR);
    d->slot[i].gen = 0;
    d->used++;
    return &d->slot[i];
}

fsck_disk_t *fsck_disk_copy(const fsck_disk_t *src) {
    fsck_disk_t *d = xcalloc(1, sizeof(*d));
    d->sectors = src->sectors;
    d->cap = src->cap;
    d->used = src->used;
    d->slot = xcalloc(d->cap, sizeof(slot_t));
    for (size_t i = 0; i < d->cap; i++) {
        if (!src->slot[i].data)
            continue;
        d->slot[i].lba = src->slot[i].lba;
        d->slot[i].data = xcalloc(1, SECTOR);
        memcpy(d->slot[i].data, src->slot[i].data, SECTOR);
    }
    return d;
}

void fsck_disk_get(const fsck_disk_t *d, uint64_t lba, uint8_t *buf) {
    const slot_t *s = lba < d->sectors ? slot_find(d, lba) : NULL;
    if (s)
        memcpy(buf, s->data, SECTOR);
    else
        memset(buf, 0, SECTOR);
}

static void journal(fsck_disk_t *d, slot_t *s) {
    if (!d->snap || s->gen == d->gen)
        return;
    if (d->undo_len == d->undo_cap) {
        d->undo_cap = d->undo_cap ? 2 * d->undo_cap : 64;
        undo_t *u = realloc(d->undo, d->undo_cap * sizeof(undo_t));
        if (!u) {
            fprintf(stderr, "fsck_disk: out of memory\n");
            abort();
        }
        d->undo = u;
    }
    d->undo[d->undo_len].lba = s->lba;
    memcpy(d->undo[d->undo_len].data, s->data, SECTOR);
    d->undo_len++;
    s->gen = d->gen;
}

void fsck_disk_put(fsck_disk_t *d, uint64_t lba, const uint8_t *buf) {
    static const uint8_t zero[SECTOR];
    if (lba >= d->sectors)
        return;
    slot_t *s = slot_find(d, lba);
    if (!s) {
        if (!memcmp(buf, zero, SECTOR))
            return;                     // still all zeros: nothing to keep
        s = slot_add(d, lba);           // zeros, which is what the journal saves
    }
    journal(d, s);
    memcpy(s->data, buf, SECTOR);
}

bool fsck_disk_read(void *ctx, uint64_t lba, uint32_t count, uint8_t *buf) {
    const fsck_disk_t *d = ctx;
    if (lba > d->sectors || count > d->sectors - lba)
        return false;
    for (uint32_t i = 0; i < count; i++)
        fsck_disk_get(d, lba + i, buf + (size_t)SECTOR * i);
    return true;
}

bool fsck_disk_write(void *ctx, uint64_t lba, uint32_t count, const uint8_t *buf) {
    fsck_disk_t *d = ctx;
    if (lba > d->sectors || count > d->sectors - lba)
        return false;
    for (uint32_t i = 0; i < count; i++)
        fsck_disk_put(d, lba + i, buf + (size_t)SECTOR * i);
    return true;
}

// Every sector `a` holds is the same in `b`.
static bool covers(const fsck_disk_t *a, const fsck_disk_t *b) {
    uint8_t s[SECTOR];
    for (size_t i = 0; i < a->cap; i++) {
        if (!a->slot[i].data)
            continue;
        fsck_disk_get(b, a->slot[i].lba, s);
        if (memcmp(s, a->slot[i].data, SECTOR) != 0)
            return false;
    }
    return true;
}

bool fsck_disk_equal(const fsck_disk_t *a, const fsck_disk_t *b) {
    return a->sectors == b->sectors && covers(a, b) && covers(b, a);
}

void fsck_disk_snapshot(fsck_disk_t *d) {
    d->snap = true;
    d->undo_len = 0;
    if (++d->gen == 0)
        d->gen = 1;
}

void fsck_disk_restore(fsck_disk_t *d) {
    if (!d->snap)
        return;
    for (size_t i = d->undo_len; i-- > 0;) {
        slot_t *s = slot_find(d, d->undo[i].lba);
        if (s)
            memcpy(s->data, d->undo[i].data, SECTOR);
    }
    d->undo_len = 0;
    if (++d->gen == 0)                  // every saved sector is stale again
        d->gen = 1;
}

size_t fsck_disk_changed(const fsck_disk_t *d, uint64_t *out, size_t max) {
    size_t n = 0;
    if (!d->snap)
        return 0;
    for (size_t i = 0; i < d->undo_len; i++) {
        const slot_t *s = slot_find(d, d->undo[i].lba);
        if (!s || !memcmp(s->data, d->undo[i].data, SECTOR))
            continue;
        if (n < max)
            out[n] = d->undo[i].lba;
        n++;
    }
    return n;
}

// --- FatFs's side: diskio, time, memory -----------------------------------------------------

static fsck_disk_t *cur;            // the disk FatFs sees as drive 0
static FATFS fs;
static bool mounted;

DSTATUS disk_initialize(BYTE pdrv) {
    return pdrv == 0 && cur ? 0 : STA_NOINIT;
}

DSTATUS disk_status(BYTE pdrv) {
    return pdrv == 0 && cur ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv || !cur)
        return RES_PARERR;
    return fsck_disk_read(cur, sector, count, buff) ? RES_OK : RES_PARERR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv || !cur)
        return RES_PARERR;
    return fsck_disk_write(cur, sector, count, buff) ? RES_OK : RES_PARERR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv || !cur)
        return RES_PARERR;
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = cur->sectors;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

DWORD get_fattime(void) {
    return (DWORD)(46u << 25 | 11u << 21 | 27u << 16);     // 2026-11-27 00:00
}

void *ff_memalloc(UINT msize) {
    return malloc(msize);
}

void ff_memfree(void *mblock) {
    free(mblock);
}

void fsck_disk_attach(fsck_disk_t *d) {
    if (mounted)
        fsck_disk_unmount();
    cur = d;
}

bool fsck_disk_mount(fsck_disk_t *d) {
    fsck_disk_attach(d);
    mounted = f_mount(&fs, "0:", 1) == FR_OK;
    if (!mounted)
        f_mount(NULL, "0:", 0);
    return mounted;
}

void fsck_disk_unmount(void) {
    f_mount(NULL, "0:", 0);
    mounted = false;
    cur = NULL;
}

FATFS *fsck_disk_fs(void) {
    return mounted ? &fs : NULL;
}

// --- the builder ----------------------------------------------------------------------------

static uint8_t work[32768];         // f_mkfs's work area, as `sd format` gives it (32 KB)

fsck_disk_t *fsck_disk_format(uint64_t sectors, BYTE fmt, BYTE n_fat, DWORD au) {
    fsck_disk_t *d = fsck_disk_new(sectors);
    fsck_disk_attach(d);
    MKFS_PARM opt = {.fmt = fmt, .n_fat = n_fat, .align = FSCK_DISK_ALIGN, .au_size = au};
    FRESULT fr = f_mkfs("0:", &opt, work, sizeof(work));
    fsck_disk_attach(NULL);
    if (fr != FR_OK) {
        fprintf(stderr, "fsck_disk: f_mkfs: %d\n", (int)fr);
        fsck_disk_free(d);
        return NULL;
    }
    return d;
}

fsck_disk_t *fsck_disk_fat32(BYTE n_fat) {
    return fsck_disk_format(FSCK_DISK_SMALL_SECTORS, FM_FAT32, n_fat, 1024);
}

uint8_t fsck_disk_pattern(const char *path, uint32_t salt, uint32_t pos) {
    uint32_t h = 2166136261u ^ salt;
    for (const char *p = path; *p; p++)
        h = (h ^ (uint8_t)*p) * 16777619u;
    uint32_t x = h ^ (pos * 0x9E3779B1u) ^ (pos >> 7);
    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= x >> 12;
    return (uint8_t)x;
}

bool fsck_disk_put_file(const char *path, uint32_t size, uint32_t salt) {
    static uint8_t buf[4096];
    FIL fp;
    if (f_open(&fp, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return false;
    bool ok = true;
    for (uint32_t done = 0; ok && done < size;) {
        UINT n = size - done < sizeof(buf) ? size - done : (UINT)sizeof(buf), wrote = 0;
        for (UINT i = 0; i < n; i++)
            buf[i] = fsck_disk_pattern(path, salt, done + i);
        ok = f_write(&fp, buf, n, &wrote) == FR_OK && wrote == n;
        done += n;
    }
    return f_close(&fp) == FR_OK && ok;
}

bool fsck_disk_populate(void) {
    static const char *const dirs[] = {
        "/dir", "/dir/sub", "/dir/emptydir", "/var", "/var/log", "/many", "/A long directory name",
    };
    static const struct {
        const char *path;
        uint32_t size;
    } files[] = {
        {"/README.TXT", 100},
        {"/notes.txt", 5000},
        {"/A long file name.txt", 10000},
        {"/dir/sub/deep.bin", 20000},
        {"/dir/sub/caf\xc3\xa9 \xc3\xbcn\xc3\xaf" "code.txt", 3000},
        {"/dir/f1.txt", 1024},
        {"/dir/f2.txt", 8193},
        {"/dir/empty.txt", 0},
        {"/var/log/messages", 70000},
        {"/var/log/messages.1", 33000},
        {"/A long directory name/one cluster.dat", 1000},
        {"/A long directory name/two clusters.dat", 1025},
        {"/A long directory name/big.dat", 150000},
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        if (f_mkdir(dirs[i]) != FR_OK)
            return false;
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        if (!fsck_disk_put_file(files[i].path, files[i].size, 0))
            return false;
    for (uint32_t i = 0; i < 30; i++) {         // /many spans several directory clusters
        char name[64];
        snprintf(name, sizeof(name), "/many/Long name number %03u.txt", (unsigned)i);
        if (!fsck_disk_put_file(name, 1 + 700 * (i % 5), i))
            return false;
    }
    return true;
}

// --- walking the tree -----------------------------------------------------------------------

#define PATH_MAX_ 512

bool fsck_disk_walk(fsck_disk_walk_fn fn, void *ctx) {
    // A queue of directory paths still to read, starting with the root.
    char (*queue)[PATH_MAX_] = xcalloc(16, PATH_MAX_);
    size_t head = 0, tail = 1, cap = 16;
    static DIR dir;
    static FILINFO info;
    static FIL fp;
    static char path[PATH_MAX_];
    bool ok = true;
    strcpy(queue[0], "/");
    while (ok && head < tail) {
        const char *base = queue[head++];
        if (f_opendir(&dir, base) != FR_OK) {
            ok = false;
            break;
        }
        while (ok) {
            if (f_readdir(&dir, &info) != FR_OK) {
                ok = false;
                break;
            }
            if (!info.fname[0])
                break;
            int n = snprintf(path, sizeof(path), "%s%s%s", base, strcmp(base, "/") ? "/" : "",
                             info.fname);
            if (n < 0 || (size_t)n >= sizeof(path)) {
                ok = false;
                break;
            }
            if (info.fattrib & AM_DIR) {
                ok = fn(path, &info, NULL, ctx);
                if (tail == cap) {
                    cap *= 2;
                    char (*q)[PATH_MAX_] = realloc(queue, cap * PATH_MAX_);
                    if (!q)
                        abort();
                    queue = q;
                    base = queue[head - 1];
                }
                memcpy(queue[tail++], path, PATH_MAX_);
            } else {
                if (f_open(&fp, path, FA_READ) != FR_OK) {
                    ok = false;
                    break;
                }
                ok = fn(path, &info, &fp, ctx);
                f_close(&fp);
            }
        }
        f_closedir(&dir);
    }
    free(queue);
    return ok;
}
