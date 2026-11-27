// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The damage generator (fsck_damage.h).

#include "fsck_damage.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz/exfat_damage.h"

#define SECTOR 512u

static uint32_t rd16(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

// --- the volume -----------------------------------------------------------------------------

static bool is_fat32_vbr(const uint8_t *s) {
    return (s[0] == 0xEB || s[0] == 0xE9 || s[0] == 0xE8) && s[510] == 0x55 && s[511] == 0xAA &&
           !memcmp(s + 82, "FAT32   ", 8);
}

bool damage_fat32_parse(const fsck_disk_t *d, damage_fat32_t *g) {
    uint8_t s[SECTOR];
    memset(g, 0, sizeof(*g));
    fsck_disk_get(d, 0, s);
    if (!is_fat32_vbr(s)) {
        if (s[510] != 0x55 || s[511] != 0xAA || s[446 + 4] == 0xEE)
            return false;
        g->vol = rd32(s + 446 + 8);
        fsck_disk_get(d, g->vol, s);
        if (!is_fat32_vbr(s))
            return false;
    }
    uint32_t bps = rd16(s + 11), reserved = rd16(s + 14), total = rd32(s + 32);
    g->spc = s[13];
    g->nfats = s[16];
    g->fatsz = rd32(s + 36);
    g->root = rd32(s + 44);
    g->fsinfo = rd16(s + 48);
    if (bps != SECTOR || !g->spc || (g->spc & (g->spc - 1)) || !reserved || !g->nfats ||
        g->nfats > 2 || !g->fatsz)
        return false;
    uint64_t meta = reserved + (uint64_t)g->nfats * g->fatsz;
    if (total <= meta)
        return false;
    g->fatbase = g->vol + reserved;
    g->database = g->vol + meta;
    g->count = (uint32_t)((total - meta) / g->spc);
    if (g->count > ((uint64_t)g->fatsz * 128) - 2)
        return false;
    return g->count > 65525;
}

uint32_t damage_fat_get(const fsck_disk_t *d, const damage_fat32_t *g, uint32_t copy,
                        uint32_t c) {
    uint8_t s[SECTOR];
    fsck_disk_get(d, g->fatbase + (uint64_t)copy * g->fatsz + c / 128, s);
    return rd32(s + 4 * (c % 128)) & 0x0FFFFFFFu;
}

void damage_fat_set(fsck_disk_t *d, const damage_fat32_t *g, uint32_t copy, uint32_t c,
                    uint32_t v) {
    uint8_t s[SECTOR];
    for (uint32_t k = 0; k < g->nfats; k++) {
        if (copy != DAMAGE_ALL_COPIES && copy != k)
            continue;
        uint64_t lba = g->fatbase + (uint64_t)k * g->fatsz + c / 128;
        fsck_disk_get(d, lba, s);
        uint8_t *e = s + 4 * (c % 128);
        wr32(e, (rd32(e) & 0xF0000000u) | (v & 0x0FFFFFFFu));
        fsck_disk_put(d, lba, s);
    }
}

uint32_t damage_chain(const fsck_disk_t *d, const damage_fat32_t *g, uint32_t first,
                      uint32_t *out, uint32_t max, bool *ended) {
    uint32_t n = 0, c = first;
    *ended = false;
    while (c >= 2 && c <= g->count + 1 && n <= g->count) {
        if (out && n < max)
            out[n] = c;
        n++;
        uint32_t next = damage_fat_get(d, g, 0, c);
        if (next >= 0x0FFFFFF8u) {
            *ended = true;
            break;
        }
        c = next;
        if (c < 2 || c > g->count + 1)   // free (0), bad (0x0FFFFFF7) or out of range
            break;
    }
    return n;
}

uint32_t damage_fsinfo_free(const fsck_disk_t *d, const damage_fat32_t *g) {
    uint8_t s[SECTOR];
    if (!g->fsinfo)
        return UINT32_MAX;
    fsck_disk_get(d, g->vol + g->fsinfo, s);
    if (rd32(s) != 0x41615252u || rd32(s + 484) != 0x61417272u || rd32(s + 508) != 0xAA550000u)
        return UINT32_MAX;
    return rd32(s + 488);
}

static void fsinfo_write(fsck_disk_t *d, const damage_fat32_t *g, uint32_t v) {
    uint8_t s[SECTOR];
    fsck_disk_get(d, g->vol + g->fsinfo, s);
    wr32(s + 488, v);
    fsck_disk_put(d, g->vol + g->fsinfo, s);
}

uint32_t damage_count_free(const fsck_disk_t *d, const damage_fat32_t *g) {
    uint8_t s[SECTOR];
    uint32_t n = 0;
    for (uint32_t c = 2; c <= g->count + 1; c++) {
        if (c == 2 || c % 128 == 0)
            fsck_disk_get(d, g->fatbase + c / 128, s);
        if (!(rd32(s + 4 * (c % 128)) & 0x0FFFFFFFu))
            n++;
    }
    return n;
}

// --- exFAT ----------------------------------------------------------------------------------

static bool is_exfat_vbr(const uint8_t *s) {
    return s[0] == 0xEB && !memcmp(s + 3, "EXFAT   ", 8) && s[510] == 0x55 && s[511] == 0xAA;
}

bool damage_exfat_parse(const fsck_disk_t *d, damage_exfat_t *g) {
    uint8_t s[SECTOR];
    memset(g, 0, sizeof(*g));
    fsck_disk_get(d, 0, s);
    if (!is_exfat_vbr(s)) {
        if (s[510] != 0x55 || s[511] != 0xAA || s[446 + 4] == 0xEE)
            return false;
        g->vol = rd32(s + 446 + 8);
        fsck_disk_get(d, g->vol, s);
        if (!is_exfat_vbr(s))
            return false;
    }
    if (s[108] != 9 || s[109] > 15 || s[110] != 1)
        return false;
    g->fatbase = g->vol + rd32(s + 80);
    g->heap = g->vol + rd32(s + 88);
    g->count = rd32(s + 92);
    g->root = rd32(s + 96);
    g->spc = 1u << s[109];
    if (g->root < 2 || g->root > g->count + 1)
        return false;
    // The bitmap entry, in the root's first cluster (where FatFs looks).
    uint64_t root = g->heap + (uint64_t)(g->root - 2) * g->spc;
    for (uint32_t i = 0; i < g->spc * (SECTOR / 32); i++) {
        if (i % (SECTOR / 32) == 0)
            fsck_disk_get(d, root + i / (SECTOR / 32), s);
        const uint8_t *e = s + 32 * (i % (SECTOR / 32));
        if (e[0] == 0x81) {
            uint32_t first = rd32(e + 20);
            if (first < 2 || first > g->count + 1)
                return false;
            g->bitmap = g->heap + (uint64_t)(first - 2) * g->spc;
            return true;
        }
    }
    return false;
}

uint32_t damage_xfat_get(const fsck_disk_t *d, const damage_exfat_t *g, uint32_t c) {
    uint8_t s[SECTOR];
    fsck_disk_get(d, g->fatbase + c / 128, s);
    return rd32(s + 4 * (c % 128));
}

void damage_xfat_set(fsck_disk_t *d, const damage_exfat_t *g, uint32_t c, uint32_t v) {
    uint8_t s[SECTOR];
    fsck_disk_get(d, g->fatbase + c / 128, s);
    wr32(s + 4 * (c % 128), v);
    fsck_disk_put(d, g->fatbase + c / 128, s);
}

bool damage_bit_get(const fsck_disk_t *d, const damage_exfat_t *g, uint32_t c) {
    uint8_t s[SECTOR];
    uint32_t bit = c - 2;
    fsck_disk_get(d, g->bitmap + bit / 4096, s);
    return s[bit / 8 % SECTOR] >> (bit % 8) & 1;
}

void damage_bit_set(fsck_disk_t *d, const damage_exfat_t *g, uint32_t c, bool set) {
    uint8_t s[SECTOR];
    uint32_t bit = c - 2;
    fsck_disk_get(d, g->bitmap + bit / 4096, s);
    uint8_t m = (uint8_t)(1u << (bit % 8));
    s[bit / 8 % SECTOR] = (uint8_t)(set ? s[bit / 8 % SECTOR] | m : s[bit / 8 % SECTOR] & ~m);
    fsck_disk_put(d, g->bitmap + bit / 4096, s);
}

uint32_t damage_needed(const damage_fat32_t *g, uint64_t size) {
    uint64_t cs = (uint64_t)g->spc * SECTOR;
    return (uint32_t)((size + cs - 1) / cs);
}

// --- the catalogue --------------------------------------------------------------------------

static bool cat_visit(const char *path, const FILINFO *info, FIL *fp, void *ctx) {
    damage_catalog_t *cat = ctx;
    damage_entry_t *e = realloc(cat->e, (cat->n + 1) * sizeof(damage_entry_t));
    if (!e)
        return false;
    cat->e = e;
    e = &cat->e[cat->n++];
    memset(e, 0, sizeof(*e));
    snprintf(e->path, sizeof(e->path), "%s", path);
    e->dir = !fp;
    if (fp) {
        e->clust = fp->obj.sclust;
        e->size = fp->obj.objsize;
        e->dir_lba = fp->dir_sect;
        e->dir_ofs = (uint32_t)(fp->dir_ptr - fp->obj.fs->win);
        e->contig = fp->obj.fs->fs_type == FS_EXFAT && (fp->obj.stat & 3) == 2;
    } else {
        // A directory's first cluster, from its entry (FAT32: high word at 20, low at 26).
        DIR dj;
        if (f_opendir(&dj, path) != FR_OK)
            return false;
        e->clust = dj.obj.sclust;
        e->contig = dj.obj.fs->fs_type == FS_EXFAT && (dj.obj.stat & 3) == 2;
        f_closedir(&dj);
    }
    (void)info;
    return true;
}

bool damage_catalog(fsck_disk_t *d, damage_catalog_t *cat) {
    memset(cat, 0, sizeof(*cat));
    if (!fsck_disk_mount(d))
        return false;
    bool ok = fsck_disk_walk(cat_visit, cat);
    fsck_disk_unmount();
    return ok;
}

void damage_catalog_free(damage_catalog_t *cat) {
    free(cat->e);
    memset(cat, 0, sizeof(*cat));
}

int damage_catalog_find(const damage_catalog_t *cat, const char *path) {
    for (size_t i = 0; i < cat->n; i++)
        if (!strcmp(cat->e[i].path, path))
            return (int)i;
    return -1;
}

// --- helpers for the kinds ------------------------------------------------------------------

static uint64_t rng_next(uint64_t *s) {         // splitmix64
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static uint32_t rng_below(damage_ctx_t *c, uint32_t n) {
    return n ? (uint32_t)(rng_next(&c->rng) % n) : 0;
}

static bool is_reserved(const damage_ctx_t *c, uint32_t cl) {
    for (size_t i = 0; i < c->n_reserved; i++)
        if (c->reserved[i] == cl)
            return true;
    return false;
}

static void reserve(damage_ctx_t *c, uint32_t cl) {
    if (c->n_reserved < sizeof(c->reserved) / sizeof(c->reserved[0]))
        c->reserved[c->n_reserved++] = cl;
}

static uint32_t clusters_of(const damage_ctx_t *c) {
    return c->exfat ? c->ex.count : c->fat.count;
}

static uint32_t needed(const damage_ctx_t *c, uint64_t size) {
    uint64_t cs = (uint64_t)(c->exfat ? c->ex.spc : c->fat.spc) * SECTOR;
    return (uint32_t)((size + cs - 1) / cs);
}

// Free: FAT32's entry 0, exFAT's bit clear.
static bool is_free(const damage_ctx_t *c, uint32_t cl) {
    return c->exfat ? !damage_bit_get(c->disk, &c->ex, cl)
                    : damage_fat_get(c->disk, &c->fat, 0, cl) == 0;
}

// Cluster `cl`'s FAT entry set to `v`, in every copy.
static void set_next(damage_ctx_t *c, uint32_t cl, uint32_t v) {
    if (c->exfat)
        damage_xfat_set(c->disk, &c->ex, cl, v);
    else
        damage_fat_set(c->disk, &c->fat, DAMAGE_ALL_COPIES, cl, v);
}

// Cluster `cl` freed as FatFs would free it: FAT32's entry 0, exFAT's bit cleared.
static void release(damage_ctx_t *c, uint32_t cl) {
    if (c->exfat)
        damage_bit_set(c->disk, &c->ex, cl, false);
    else
        damage_fat_set(c->disk, &c->fat, DAMAGE_ALL_COPIES, cl, 0);
}

static void add_cluster(damage_fault_t *f, uint32_t cl) {
    if (f->n_clusters < DAMAGE_CLUSTERS_MAX)
        f->clusters[f->n_clusters++] = cl;
}

// Keep FSInfo's free count true after `delta` clusters were freed (+) or allocated (-).
static void fsinfo_adjust(damage_ctx_t *c, int32_t delta) {
    if (c->exfat)
        return;
    uint32_t v = damage_fsinfo_free(c->disk, &c->fat);
    if (v != UINT32_MAX)
        fsinfo_write(c->disk, &c->fat, (uint32_t)((int64_t)v + delta));
}

// A run of `k` consecutive free clusters from a random place (wrapping once), none of them
// reserved. Returns its first cluster, or 0.
static uint32_t free_run(damage_ctx_t *c, uint32_t k) {
    uint32_t count = clusters_of(c);
    uint32_t start = 2 + rng_below(c, count);
    for (uint32_t tried = 0; tried < count; tried++) {
        uint32_t first = 2 + (start - 2 + tried) % count;
        if (first + k - 1 > count + 1)
            continue;
        uint32_t i = 0;
        while (i < k && !is_reserved(c, first + i) && is_free(c, first + i))
            i++;
        if (i == k)
            return first;
        tried += i;                     // the run broke at first + i: skip past it
    }
    return 0;
}

// Link `k` clusters from `first`, ending the chain, in every FAT copy (exFAT: their bits set
// too); reserve them.
static void link_run(damage_ctx_t *c, uint32_t first, uint32_t k, damage_fault_t *f) {
    for (uint32_t i = 0; i < k; i++) {
        set_next(c, first + i, i + 1 < k ? first + i + 1 : c->exfat ? DAMAGE_EXFAT_EOC : DAMAGE_EOC);
        if (c->exfat)
            damage_bit_set(c->disk, &c->ex, first + i, true);
        reserve(c, first + i);
        add_cluster(f, first + i);
    }
    fsinfo_adjust(c, -(int32_t)k);
}

// An exFAT file's clusters: its run (NoFatChain), or its chain through the 32-bit FAT.
static uint32_t xchain(const damage_ctx_t *c, const damage_entry_t *e, uint32_t *out,
                       uint32_t max, bool *ended) {
    const damage_exfat_t *g = &c->ex;
    uint32_t n = 0, cl = e->clust;
    *ended = false;
    if (e->contig) {
        uint32_t need = needed(c, e->size);
        *ended = cl >= 2 && (uint64_t)cl + need <= (uint64_t)g->count + 2;
        for (; *ended && n < need; n++)
            if (n < max)
                out[n] = cl + n;
        return n;
    }
    while (cl >= 2 && cl <= g->count + 1 && n <= g->count) {
        if (n < max)
            out[n] = cl;
        n++;
        uint32_t next = damage_xfat_get(c->disk, g, cl);
        if (next >= 0xFFFFFFF8u) {
            *ended = true;
            break;
        }
        cl = next;
    }
    return n;
}

// A random file no fault involves yet, whose chain ends properly and has at least `min_len`
// clusters (and matches its size), after entry `after` (-1: anywhere); `contig`: on exFAT, 1
// for a NoFatChain file only, 0 for a FAT-chained one only, -1 either. Its chain goes to
// `chain` (up to `max`). Returns its entry, or -1.
static int pick_file(damage_ctx_t *c, uint32_t min_len, int after, int contig, uint32_t *chain,
                     uint32_t max, uint32_t *len) {
    size_t n = c->cat->n;
    if (!n)
        return -1;
    uint32_t start = rng_below(c, (uint32_t)n);
    for (size_t k = 0; k < n; k++) {
        size_t i = (start + k) % n;
        const damage_entry_t *e = &c->cat->e[i];
        if (e->dir || c->used[i] || (int)i <= after || !e->clust
                || (contig >= 0 && e->contig != (contig > 0)))
            continue;
        bool ended;
        uint32_t l = c->exfat ? xchain(c, e, chain, max, &ended)
                              : damage_chain(c->disk, &c->fat, e->clust, chain, max, &ended);
        if (!ended || l < min_len || l != needed(c, e->size) || l > max)
            continue;
        *len = l;
        return (int)i;
    }
    return -1;
}

static void use(damage_ctx_t *c, damage_fault_t *f, unsigned slot, int file, bool broken) {
    c->used[file] = true;
    f->file[slot] = file;
    f->broken[slot] = broken;
}

static const char *path_of(const damage_ctx_t *c, int file) {
    return c->cat->e[file].path;
}

#define CHAIN_MAX 512u              // longest chain a kind works on (150 KB files at 1 KB)

// The files the shared kinds change the chain of: any on FAT32, FAT-chained ones on exFAT.
static int chained(const damage_ctx_t *c) {
    return c->exfat ? 0 : -1;
}

// --- the FAT32 kinds ------------------------------------------------------------------------

static bool lost_chain(damage_ctx_t *c, damage_fault_t *f) {
    uint32_t k = 1 + rng_below(c, 4);
    uint32_t first = free_run(c, k);
    if (!first)
        return false;
    link_run(c, first, k, f);
    snprintf(f->what, sizeof(f->what), "lost chain: clusters %" PRIu32 "-%" PRIu32, first,
             first + k - 1);
    return true;
}

static bool overlong(damage_ctx_t *c, damage_fault_t *f) {
    static uint32_t chain[CHAIN_MAX];
    uint32_t len;
    int a = pick_file(c, 1, -1, chained(c), chain, CHAIN_MAX, &len);
    if (a < 0)
        return false;
    uint32_t k = 1 + rng_below(c, 3);
    uint32_t first = free_run(c, k);
    if (!first)
        return false;
    use(c, f, 0, a, false);
    set_next(c, chain[len - 1], first);
    reserve(c, chain[len - 1]);
    add_cluster(f, chain[len - 1]);
    link_run(c, first, k, f);
    f->value[0] = len;
    f->value[1] = k;
    snprintf(f->what, sizeof(f->what), "%.60s: chain of %" PRIu32 " grown by %" PRIu32
             " (clusters %" PRIu32 "-%" PRIu32 ")", path_of(c, a), len, k, first, first + k - 1);
    return true;
}

static bool stale_fsinfo(damage_ctx_t *c, damage_fault_t *f) {
    uint32_t v = damage_fsinfo_free(c->disk, &c->fat);
    if (c->fsinfo_stale || v == UINT32_MAX || v > c->fat.count)
        return false;
    uint32_t delta = 1 + rng_below(c, 1000);
    uint32_t stale = (rng_below(c, 2) && v >= delta) || v + delta > c->fat.count ? v - delta
                                                                                 : v + delta;
    fsinfo_write(c->disk, &c->fat, stale);
    c->fsinfo_stale = true;
    f->value[0] = v;
    f->value[1] = stale;
    snprintf(f->what, sizeof(f->what), "FSInfo free count %" PRIu32 " written as %" PRIu32, v,
             stale);
    return true;
}

static bool fat_copies(damage_ctx_t *c, damage_fault_t *f) {
    if (c->fat.nfats < 2)
        return false;
    uint32_t x = free_run(c, 1);
    if (!x)
        return false;
    damage_fat_set(c->disk, &c->fat, 1, x, DAMAGE_EOC);
    reserve(c, x);
    add_cluster(f, x);
    snprintf(f->what, sizeof(f->what), "FAT copy 1: free cluster %" PRIu32 " marked used", x);
    return true;
}

static bool cross_link(damage_ctx_t *c, damage_fault_t *f) {
    static uint32_t ca[CHAIN_MAX], cb[CHAIN_MAX];
    uint32_t la, lb;
    int a = pick_file(c, 2, -1, chained(c), ca, CHAIN_MAX, &la);
    if (a < 0)
        return false;
    c->used[a] = true;                  // so B isn't A
    int b = pick_file(c, 1, -1, chained(c), cb, CHAIN_MAX, &lb);
    c->used[a] = false;
    if (b < 0 || la < 2 || la > CHAIN_MAX || !lb || lb > CHAIN_MAX)
        return false;
    // The redirected cluster: one of A's last 15 but its last, so the freed tail is recorded.
    uint32_t span = la - 1 < DAMAGE_CLUSTERS_MAX - 2 ? la - 1 : DAMAGE_CLUSTERS_MAX - 2;
    uint32_t i = la - 2 - rng_below(c, span), j = rng_below(c, lb);
    use(c, f, 0, a, true);
    use(c, f, 1, b, false);
    set_next(c, ca[i], cb[j]);
    reserve(c, ca[i]);
    add_cluster(f, ca[i]);
    add_cluster(f, cb[j]);
    for (uint32_t k = i + 1; k < la; k++) {         // A's old tail: freed
        release(c, ca[k]);
        reserve(c, ca[k]);
        add_cluster(f, ca[k]);
    }
    fsinfo_adjust(c, (int32_t)(la - 1 - i));
    f->value[0] = i + 1 + lb - j;
    f->value[1] = la;
    snprintf(f->what, sizeof(f->what), "%.60s: cluster %" PRIu32 " now points into %.60s at %"
             PRIu32 " (chain %" PRIu32 ", needs %" PRIu32 ")", path_of(c, a), ca[i], path_of(c, b),
             cb[j], f->value[0], la);
    return true;
}

// FAT32: the cluster is free then, and those after it lost. exFAT: its bit stays set, so it is
// lost too.
static bool broken_chain(damage_ctx_t *c, damage_fault_t *f) {
    static uint32_t ca[CHAIN_MAX];
    uint32_t la;
    int a = pick_file(c, 1, -1, chained(c), ca, CHAIN_MAX, &la);
    if (a < 0)
        return false;
    uint32_t i = rng_below(c, la);
    use(c, f, 0, a, true);
    set_next(c, ca[i], 0);
    fsinfo_adjust(c, 1);
    reserve(c, ca[i]);
    for (uint32_t k = i; k < la; k++)
        add_cluster(f, ca[k]);
    f->value[0] = la - 1 - i + (c->exfat ? 1 : 0);
    snprintf(f->what, sizeof(f->what), "%.60s: cluster %" PRIu32 " (%" PRIu32 " of %" PRIu32
             ") marked free, %" PRIu32 " after it lost", path_of(c, a), ca[i], i + 1, la,
             f->value[0]);
    return true;
}

static bool x_size_larger(damage_ctx_t *c, damage_fault_t *f, int a, uint64_t size);

static bool size_larger(damage_ctx_t *c, damage_fault_t *f) {
    static uint32_t ca[CHAIN_MAX];
    uint32_t la;
    int a = pick_file(c, 1, -1, chained(c), ca, CHAIN_MAX, &la);
    if (a < 0)
        return false;
    const damage_entry_t *e = &c->cat->e[a];
    uint32_t cs = (c->exfat ? c->ex.spc : c->fat.spc) * SECTOR;
    uint64_t size = (uint64_t)la * cs + 1 + rng_below(c, 2 * cs);
    if (size > UINT32_MAX)
        return false;
    if (c->exfat)
        return x_size_larger(c, f, a, size);
    uint8_t s[SECTOR];
    fsck_disk_get(c->disk, e->dir_lba, s);
    if (e->dir_ofs > SECTOR - 32 || rd32(s + e->dir_ofs + 28) != e->size)
        return false;
    use(c, f, 0, a, true);
    wr32(s + e->dir_ofs + 28, (uint32_t)size);
    fsck_disk_put(c->disk, e->dir_lba, s);
    f->value[0] = (uint32_t)e->size;
    f->value[1] = (uint32_t)size;
    snprintf(f->what, sizeof(f->what), "%.60s: size %" PRIu64 " written as %" PRIu64
             " (chain holds %" PRIu64 ")", path_of(c, a), e->size, size, (uint64_t)la * cs);
    return true;
}

static bool overlong_xlink(damage_ctx_t *c, damage_fault_t *f) {
    static uint32_t ca[CHAIN_MAX], cb[CHAIN_MAX];
    uint32_t la, lb;
    int a = -1, b = -1;
    // A must have a later file B: try a few starting points.
    for (int tries = 0; tries < 8 && b < 0; tries++) {
        a = pick_file(c, 1, -1, chained(c), ca, CHAIN_MAX, &la);
        if (a < 0)
            return false;
        b = pick_file(c, 1, a, chained(c), cb, CHAIN_MAX, &lb);
    }
    if (b < 0)
        return false;
    uint32_t j = rng_below(c, lb);
    use(c, f, 0, a, false);
    use(c, f, 1, b, false);
    set_next(c, ca[la - 1], cb[j]);
    reserve(c, ca[la - 1]);
    add_cluster(f, ca[la - 1]);
    add_cluster(f, cb[j]);
    f->value[0] = la;
    f->value[1] = lb - j;
    snprintf(f->what, sizeof(f->what), "%.60s: last cluster %" PRIu32 " now runs into %.60s at %"
             PRIu32 " (%" PRIu32 " extra)", path_of(c, a), ca[la - 1], path_of(c, b), cb[j],
             lb - j);
    return true;
}

const damage_kind_t damage_fat32_kinds[DAMAGE_FAT32_KINDS] = {
    [DAMAGE_LOST_CHAIN] = {"lost chain", true, lost_chain},
    [DAMAGE_OVERLONG] = {"over-long chain", true, overlong},
    [DAMAGE_STALE_FSINFO] = {"stale FSInfo", true, stale_fsinfo},
    [DAMAGE_FAT_COPIES] = {"FAT copies differ", true, fat_copies},
    [DAMAGE_CROSS_LINK] = {"cross-link", false, cross_link},
    [DAMAGE_BROKEN_CHAIN] = {"chain into a free entry", false, broken_chain},
    [DAMAGE_SIZE_LARGER] = {"size larger than chain", false, size_larger},
    [DAMAGE_OVERLONG_XLINK] = {"over-long chain cross-linked", false, overlong_xlink},
};

// --- the exFAT kinds ------------------------------------------------------------------------

static bool x_lost_run(damage_ctx_t *c, damage_fault_t *f) {
    uint32_t k = 1 + rng_below(c, 4);
    uint32_t first = free_run(c, k);
    if (!first)
        return false;
    for (uint32_t i = 0; i < k; i++) {
        damage_bit_set(c->disk, &c->ex, first + i, true);
        reserve(c, first + i);
        add_cluster(f, first + i);
    }
    snprintf(f->what, sizeof(f->what), "bits set for clusters %" PRIu32 "-%" PRIu32, first,
             first + k - 1);
    return true;
}

static bool x_reached_free(damage_ctx_t *c, damage_fault_t *f) {
    static uint32_t ca[CHAIN_MAX];
    uint32_t la;
    int a = pick_file(c, 1, -1, -1, ca, CHAIN_MAX, &la);
    if (a < 0)
        return false;
    uint32_t k = 1 + rng_below(c, la < 3 ? la : 3), i = rng_below(c, la - k + 1);
    use(c, f, 0, a, false);
    for (uint32_t n = i; n < i + k; n++) {
        damage_bit_set(c->disk, &c->ex, ca[n], false);
        reserve(c, ca[n]);
        add_cluster(f, ca[n]);
    }
    snprintf(f->what, sizeof(f->what), "%.60s: bits of %" PRIu32 " clusters cleared from %" PRIu32
             " of %" PRIu32 " (cluster %" PRIu32 ")", path_of(c, a), k, i + 1, la, ca[i]);
    return true;
}

static bool x_past_run(damage_ctx_t *c, damage_fault_t *f) {
    static uint32_t ca[CHAIN_MAX];
    uint32_t la;
    uint32_t k = 1 + rng_below(c, 3), first = 0;
    int a = -1;
    // Most NoFatChain files are followed by another file: look for one that isn't.
    bool *tried = calloc(c->cat->n ? c->cat->n : 1, sizeof(bool));
    if (!tried)
        return false;
    while (!first && (a = pick_file(c, 1, -1, 1, ca, CHAIN_MAX, &la)) >= 0) {
        first = ca[la - 1] + 1;
        for (uint32_t i = 0; first && i < k; i++)
            if (first + i > c->ex.count + 1 || is_reserved(c, first + i) || !is_free(c, first + i))
                first = 0;
        if (!first)
            c->used[a] = tried[a] = true;
    }
    for (size_t i = 0; i < c->cat->n; i++)
        if (tried[i])
            c->used[i] = false;
    free(tried);
    if (a < 0)
        return false;
    use(c, f, 0, a, false);
    for (uint32_t i = 0; i < k; i++) {
        damage_bit_set(c->disk, &c->ex, first + i, true);
        reserve(c, first + i);
        add_cluster(f, first + i);
    }
    f->value[0] = k;
    snprintf(f->what, sizeof(f->what), "%.60s: bits set for %" PRIu32 " clusters past its run (%"
             PRIu32 "-%" PRIu32 ")", path_of(c, a), k, first, first + k - 1);
    return true;
}

// exFAT entry sets are reached through exfat_damage.h, which wants writable sectors: a cache
// over the disk, written back when the fault is done.
#define XCACHE_MAX 256u

typedef struct {
    fsck_disk_t *disk;
    unsigned n;
    uint64_t lba[XCACHE_MAX];
    uint8_t data[XCACHE_MAX][SECTOR];
} xcache_t;

static uint8_t *xcache_sector(void *ctx, uint64_t lba) {
    xcache_t *x = ctx;
    for (unsigned i = 0; i < x->n; i++)
        if (x->lba[i] == lba)
            return x->data[i];
    if (x->n == XCACHE_MAX)
        abort();                        // a fault that reads more than it ever should
    x->lba[x->n] = lba;
    fsck_disk_get(x->disk, lba, x->data[x->n]);
    return x->data[x->n++];
}

static bool x_size_larger(damage_ctx_t *c, damage_fault_t *f, int a, uint64_t size) {
    const damage_entry_t *e = &c->cat->e[a];
    xcache_t *xc = calloc(1, sizeof(*xc));
    if (!xc)
        return false;
    xc->disk = c->disk;
    exd_t x;
    exd_loc_t loc;
    bool ok = exd_open(&x, xcache_sector, xc, c->ex.vol, fsck_disk_sectors(c->disk)) &&
              exd_find(&x, e->path + 1, &loc) && loc.set.length == e->size;
    if (ok) {
        exd_put64(exd_set_entry(&x, &loc, 1) + 24, size);
        exd_resum(&x, &loc);
        for (unsigned i = 0; i < xc->n; i++)
            fsck_disk_put(c->disk, xc->lba[i], xc->data[i]);
        use(c, f, 0, a, true);
        f->value[0] = (uint32_t)e->size;
        f->value[1] = (uint32_t)size;
        snprintf(f->what, sizeof(f->what), "%.60s: DataLength %" PRIu64 " written as %" PRIu64,
                 path_of(c, a), e->size, size);
    }
    free(xc);
    return ok;
}

const damage_kind_t damage_exfat_kinds[DAMAGE_EXFAT_KINDS] = {
    [DAMAGE_X_LOST_RUN] = {"bits set for nothing", true, x_lost_run},
    [DAMAGE_X_REACHED_FREE] = {"bits cleared under a file", true, x_reached_free},
    [DAMAGE_X_OVERLONG] = {"over-long FAT chain", true, overlong},
    [DAMAGE_X_PAST_RUN] = {"bits set past a NoFatChain run", true, x_past_run},
    [DAMAGE_X_CROSS_LINK] = {"cross-link", false, cross_link},
    [DAMAGE_X_BROKEN_CHAIN] = {"chain into a free entry", false, broken_chain},
    [DAMAGE_X_SIZE_LARGER] = {"size larger than chain", false, size_larger},
    [DAMAGE_X_OVERLONG_XLINK] = {"over-long chain cross-linked", false, overlong_xlink},
};

// --- applying -------------------------------------------------------------------------------

// A context on the heap (it's 4 KB), or NULL.
static damage_ctx_t *ctx_new(fsck_disk_t *d, const damage_catalog_t *cat, uint64_t seed) {
    damage_ctx_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->disk = d;
    c->cat = cat;
    c->rng = seed;
    c->used = calloc(cat->n ? cat->n : 1, sizeof(bool));
    bool fat32 = c->used && damage_fat32_parse(d, &c->fat);
    if (c->used && !fat32)
        c->exfat = damage_exfat_parse(d, &c->ex);
    if (!fat32 && !c->exfat) {
        free(c->used);
        free(c);
        return NULL;
    }
    return c;
}

static void ctx_free(damage_ctx_t *c) {
    free(c->used);
    free(c);
}

static bool apply(damage_ctx_t *c, const damage_kind_t *kind, damage_fault_t *f) {
    memset(f, 0, sizeof(*f));
    f->kind = kind;
    f->file[0] = f->file[1] = -1;
    if (kind->apply(c, f))
        return true;
    memset(f, 0, sizeof(*f));
    return false;
}

bool damage_apply(fsck_disk_t *d, const damage_catalog_t *cat, const damage_kind_t *kind,
                  uint64_t seed, damage_fault_t *out) {
    damage_ctx_t *c = ctx_new(d, cat, seed);
    if (!c)
        return false;
    bool ok = apply(c, kind, out);
    ctx_free(c);
    return ok;
}

bool damage_random(fsck_disk_t *d, const damage_catalog_t *cat, const damage_kind_t *kinds,
                   size_t n_kinds, uint64_t seed, damage_t *out) {
    memset(out, 0, sizeof(*out));
    out->seed = seed;
    damage_ctx_t *c = n_kinds ? ctx_new(d, cat, seed) : NULL;
    if (!c)
        return false;
    unsigned want = 1 + rng_below(c, DAMAGE_FAULTS_MAX);
    for (unsigned tries = 0; out->n < want && tries < 8 * DAMAGE_FAULTS_MAX; tries++) {
        const damage_kind_t *k = &kinds[rng_below(c, (uint32_t)n_kinds)];
        if (out->n == 0 && !k->repairable)
            continue;
        if (apply(c, k, &out->f[out->n]))
            out->n++;
    }
    ctx_free(c);
    return out->n > 0;
}

size_t damage_broken_paths(const damage_t *dm, const damage_catalog_t *cat, const char **out,
                           size_t max) {
    size_t n = 0;
    for (unsigned i = 0; i < dm->n; i++)
        for (unsigned s = 0; s < 2; s++)
            if (dm->f[i].file[s] >= 0 && dm->f[i].broken[s]) {
                if (n < max)
                    out[n] = cat->e[dm->f[i].file[s]].path;
                n++;
            }
    return n;
}

void damage_print(const damage_t *dm, FILE *out) {
    fprintf(out, "  damage, seed %" PRIu64 ", %u faults:\n", dm->seed, dm->n);
    for (unsigned i = 0; i < dm->n; i++)
        fprintf(out, "    %s (%s): %s\n", dm->f[i].kind->name,
                dm->f[i].kind->repairable ? "repairable" : "reported", dm->f[i].what);
}
