// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The card checker (fsck.h; design in docs/design/sd-check.md). The work is a state machine:
// every place that reads a sector can give up for lack of budget and be entered again later
// with nothing lost, so all progress lives in the fsck_t, never in locals across a read.
//
// The order of the work:
//   1. find the volume (sector 0, GPT or MBR) as FatFs's find_volume() does, then read and
//      check the boot sector as its mount_volume() does;
//   2. the boot region checks of pass 1: backup boot sector, FSInfo, FAT entries 0 and 1, and
//      the two FATs compared when there are two;
//   3. one pass per window of clusters: walk the directory tree, following every chain and
//      marking the clusters it reaches in the window, then compare the window with the FAT;
//   4. the summary.
// Per-entry problems are reported in pass 1 only; the later passes walk the same tree again
// and only mark clusters.

#include "files/fsck.h"
#include "files/fsck_repair.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define SECTOR            512u
#define DIRENT            32u
#define ENTRIES_PER_SEC   (SECTOR / DIRENT)
#define FAT_PER_SEC       (SECTOR / 4u)
#define FAT32_MASK        0x0FFFFFFFu   // FatFs ignores the top 4 bits of an entry
#define FAT32_BAD         0x0FFFFFF7u
#define FAT32_EOC         0x0FFFFFF8u   // this or above ends a chain
#define MAX_FAT16         65525u        // ff.c: more clusters than this is FAT32
#define MAX_FAT32         0x0FFFFFF5u
#define DIR_ENTRIES_MAX   65536u        // ff.c's MAX_DIR (2 MB) in entries: FatFs reads no further
#define BLOOM_BITS        4096u
#define NAME_UNITS        256u          // FF_MAX_LFN + 1: a long name and its terminator
#define PATH_LEN          256u
#define NO_LBA            UINT64_MAX

// FAT boot sector, FSInfo, MBR and GPT offsets (the names of ff.c's).
#define BPB_BytsPerSec    11
#define BPB_SecPerClus    13
#define BPB_RsvdSecCnt    14
#define BPB_NumFATs       16
#define BPB_RootEntCnt    17
#define BPB_TotSec16      19
#define BPB_Media         21
#define BPB_FATSz16       22
#define BPB_TotSec32      32
#define BPB_FATSz32       36
#define BPB_FSVer32       42
#define BPB_RootClus32    44
#define BPB_FSInfo32      48
#define BPB_BkBootSec32   50
#define BS_FilSysType32   82
#define BS_55AA           510
#define FSI_LeadSig       0
#define FSI_StrucSig      484
#define FSI_Free_Count    488
#define MBR_Table         446
#define GPTH_Size         12
#define GPTH_Bcc          16
#define GPTH_PtOfs        72
#define GPTH_PtNum        80
#define GPTH_PteSize      84
#define GPTH_PtBcc        88
#define GPTE_FstLba       32
#define SZ_GPTE           128u

// Directory entry fields.
#define DIR_Attr          11
#define DIR_NTres         12
#define DIR_FstClusHI     20
#define DIR_FstClusLO     26
#define DIR_FileSize      28
#define LDIR_Chksum       13
#define AM_VOL            0x08
#define AM_DIR            0x10
#define AM_ARC            0x20
#define AM_LFN            0x0F
#define LLEF              0x40          // last (first stored) long-name entry of a name
#define DDEM              0xE5          // deleted entry
#define RDDEM             0x05          // stands for 0xE5 as a name's first byte

static const uint8_t guid_ms_basic[16] = {0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
                                          0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};

// What the helpers return.
enum { R_OK, R_YIELD, R_FAIL, R_END };

enum {
    ST_FIND0, ST_GPT_HEADER, ST_GPT_CRC, ST_GPT_SCAN, ST_MBR, ST_BPB,
    ST_BACKUP, ST_FSINFO, ST_FAT01, ST_FATCMP,
    ST_PASS, ST_WALK, ST_COMPARE, ST_FINISH, ST_DONE, ST_FAILED,
};

// The directory walk's states.
enum { WK_ROOT, WK_DIRCHAIN, WK_SCAN, WK_RESCAN, WK_ENTRY, WK_FILECHAIN, WK_POP, WK_END };

// How a chain ended (CH_RUN: not yet).
enum { CH_RUN, CH_EOC, CH_FREE, CH_BAD, CH_RANGE, CH_START_RANGE, CH_LOOP, CH_STOPPED };

// What an entry is to the walk (lfn_feed()).
enum { K_SKIP, K_LFN, K_DOT, K_ITEM };

// The kind of entry whose chain is being followed.
enum { ENT_FILE, ENT_DEEP_DIR };

// One chain being followed through the FAT. Loops are caught without memory: up to the length
// the entry needs the chain is just counted, and beyond it Brent's cycle detection runs (a
// tortoise that jumps to the hare at powers of two), so a long correct chain costs nothing
// extra and a loop is found within about twice its length. More steps than clusters is a loop
// in any case.
typedef struct {
    uint32_t start;
    uint32_t cur;               // the cluster whose FAT entry is read next
    uint32_t n;                 // clusters counted so far
    uint32_t expected;          // clusters the entry needs; Brent starts beyond
    uint32_t tort, power, lam;  // Brent's state
    uint32_t last;              // the last cluster counted
    uint32_t keep;              // the `expected`th cluster: where repair puts a trim's end mark
    uint32_t hits;              // clusters this chain found already marked in the window
    uint32_t bad;               // the value that broke the chain
    uint8_t status;
    bool brent;
    bool last_in;               // `last` is in this pass's window...
    bool last_hit;              // ...and was already marked when this chain reached it
} chain_t;

// A position in a directory: entry `ord`, in the `ci`th cluster of its chain, `clus`.
typedef struct {
    uint32_t clus, ci, ord;
} dirpos_t;

// A directory on the walk's stack (24 bytes).
typedef struct {
    uint32_t start;             // first cluster
    uint32_t nclus;             // clusters of its chain that can be read
    dirpos_t pos;               // the next entry to look at
    uint16_t path_len;          // its path's length in path[]
    uint16_t pad;
} frame_t;

// FatFs's dir_read() state for long names, and the pending entries of a sequence.
typedef struct {
    uint8_t ord, sum;
    uint32_t pstart, plen;
} lfn_t;

struct fsck {
    fsck_io_t io;
    fsck_fix_t *fix;            // repair's hooks (fsck_repair.h); NULL for a check
    fsck_result_t res;
    uint32_t budget;            // reads left in this step
    uint64_t reads;             // sectors read in all
    uint8_t state, wk, sub;
    char why[48];

    // Finding the volume.
    unsigned vf_fmt;            // check_fs()'s answer for the last candidate
    uint32_t vf_i;
    uint32_t mbr_pt[4];
    uint64_t gpt_lba;
    uint32_t gpt_n, gpt_crc, gpt_want;
    uint64_t err_lba;

    // The volume.
    uint64_t vol_lba, fat_lba, data_lba, tsect;
    uint32_t fsize, count, csize, cbytes, epc, root, dir_expected;
    uint16_t nrsv, fsinfo, bkboot;
    uint8_t nfats, media;
    bool fsi_valid;
    uint32_t fsi_free;
    uint32_t diff_n, diff_first;

    // Sector buffers and the sector each holds.
    uint8_t fatbuf[SECTOR], dirbuf[SECTOR], aux[SECTOR];
    uint64_t fat_buf_lba, dir_lba, aux_lba;

    // Passes.
    uint8_t *win;
    uint32_t win_bits;          // clusters per window
    uint32_t pass, win_lo, win_hi;
    uint32_t cmp_next, run_start, run_len;
    uint32_t steps, steps_max;

    // The walk.
    frame_t stack[FSCK_DEPTH_MAX];
    uint32_t depth;
    chain_t ch;
    uint32_t ent_size;
    uint8_t ent_kind;
    lfn_t lf, rl;               // the scan's long names, the rescan's
    uint16_t lfn[NAME_UNITS];   // the scan's long name, then the entry's key (upcased)
    uint16_t rlfn[NAME_UNITS];  // the rescan's long name, then its key
    uint8_t key_sfn[11];
    uint8_t bloom[BLOOM_BITS / 8];
    dirpos_t rpos;
    uint64_t rescan_max;        // sectors rescans may read in this directory
    uint64_t rescan_used;       // sectors they read before the one running
    uint64_t rescan_from;       // f->reads when the one running started
    bool dup_off;
    bool seen_dot, seen_dotdot;
    uint32_t orph_start, orph_len;
    char path[PATH_LEN];
    uint16_t path_len;
    char line[FSCK_LINE_MAX];
};

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

static bool is_pow2(uint32_t v) {
    return v && !(v & (v - 1));
}

// CRC-32 as FatFs computes it for GPT (bit by bit, reflected 0xEDB88320).
static uint32_t crc32_add(uint32_t crc, uint8_t d) {
    crc ^= d;
    for (int b = 0; b < 8; b++)
        crc = (crc & 1) ? crc >> 1 ^ 0xEDB88320u : crc >> 1;
    return crc;
}

static const char *plural(uint32_t n, const char *one, const char *many) {
    return n == 1 ? one : many;
}

// "512 bytes", "32 KB", "1.5 MB": whole units, or one decimal when not whole.
static void fmt_size(char *out, size_t n, uint64_t bytes) {
    static const char *const units[] = {"bytes", "KB", "MB", "GB", "TB"};
    unsigned u = 0;
    uint64_t div = 1;
    while (u < 4 && bytes >= div * 1024)
        div *= 1024, u++;
    uint64_t whole = bytes / div, tenth = (bytes % div) * 10 / div;
    if (tenth)
        snprintf(out, n, "%" PRIu64 ".%" PRIu64 " %s", whole, tenth, units[u]);
    else
        snprintf(out, n, "%" PRIu64 " %s", whole, units[u]);
}

// One problem or note: counted always, handed to report() while under FSCK_REPORT_MAX.
__attribute__((format(printf, 3, 4)))
static void emit(fsck_t *f, bool note, const char *fmt, ...) {
    if (note)
        f->res.notes++;
    else
        f->res.problems++;
    if (f->res.reported >= FSCK_REPORT_MAX || !f->io.report)
        return;
    size_t at = 0;
    if (note) {
        memcpy(f->line, "note: ", 6);
        at = 6;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->line + at, sizeof(f->line) - at, fmt, ap);
    va_end(ap);
    f->res.reported++;
    f->io.report(f->io.ctx, f->line);
}

// The path of the entry or directory being looked at ("/" for the root).
static const char *path(const fsck_t *f) {
    return f->path_len ? f->path : "/";
}

static void refuse(fsck_t *f, const char *why) {
    f->res.why = why;
    f->state = ST_FAILED;
}

// What repair's hooks answer: FSCK_FIX_FAIL (a write failed) stops the run at once.
static int fixed(fsck_t *f, int rc) {
    if (rc == FSCK_FIX_FAIL) {
        refuse(f, fsck_fix_why(f->fix));
        return R_FAIL;
    }
    return rc == FSCK_FIX_YIELD ? R_YIELD : R_OK;
}

// One sector into `buf`, if the budget allows. R_FAIL: the device refused it, or it is past the
// device's end (f->err_lba says which).
static int rd(fsck_t *f, uint64_t lba, uint8_t *buf) {
    if (!f->budget)
        return R_YIELD;
    f->budget--;
    f->reads++;
    if (lba >= f->io.sectors || !f->io.read(f->io.ctx, lba, 1, buf)) {
        f->err_lba = lba;
        return R_FAIL;
    }
    return R_OK;
}

// FAT entry `c` (top 4 bits dropped), through the one-sector FAT cache.
static int fat_get(fsck_t *f, uint32_t c, uint32_t *v) {
    uint64_t lba = f->fat_lba + c / FAT_PER_SEC;
    if (f->fat_buf_lba != lba) {
        int r = rd(f, lba, f->fatbuf);
        if (r != R_OK) {
            if (r == R_FAIL)
                f->fat_buf_lba = NO_LBA;
            return r;
        }
        f->fat_buf_lba = lba;
    }
    *v = le32(f->fatbuf + (c % FAT_PER_SEC) * 4) & FAT32_MASK;
    return R_OK;
}

static uint64_t clus_lba(const fsck_t *f, uint32_t c) {
    return f->data_lba + (uint64_t)(c - 2) * f->csize;
}

static bool in_window(const fsck_t *f, uint32_t c) {
    return c >= f->win_lo && c < f->win_hi;
}

// Mark cluster `c` (in the window) as reached; true if it already was.
static bool mark(fsck_t *f, uint32_t c) {
    uint32_t i = c - f->win_lo;
    uint8_t m = (uint8_t)(1u << (i & 7));
    bool was = f->win[i >> 3] & m;
    f->win[i >> 3] |= m;
    return was;
}

static bool marked(const fsck_t *f, uint32_t c) {
    uint32_t i = c - f->win_lo;
    return f->win[i >> 3] & (1u << (i & 7));
}

// --- finding the volume (ff.c's check_fs(), find_volume(), test_gpt_header()) ---------------

// What a sector is: 0 a FAT VBR, 1 an exFAT VBR, 2 not a VBR but a valid boot sector (0x55AA),
// 3 neither. ff.c's check_fs() exactly, with FF_MIN_SS = FF_MAX_SS = 512.
static unsigned check_fs(const uint8_t *b) {
    uint16_t sign = le16(b + BS_55AA);
    if (sign == 0xAA55 && !memcmp(b, "\xEB\x76\x90" "EXFAT   ", 11))
        return 1;
    uint8_t j = b[0];
    if (j == 0xEB || j == 0xE9 || j == 0xE8) {
        if (sign == 0xAA55 && !memcmp(b + BS_FilSysType32, "FAT32   ", 8))
            return 0;
        // Early MS-DOS volumes have neither 0x55AA nor the type string: a plausible BPB.
        uint8_t spc = b[BPB_SecPerClus];
        if (le16(b + BPB_BytsPerSec) == SECTOR && is_pow2(spc)
                && le16(b + BPB_RsvdSecCnt) != 0 && (unsigned)b[BPB_NumFATs] - 1u <= 1u
                && le16(b + BPB_RootEntCnt) != 0
                && (le16(b + BPB_TotSec16) >= 128 || le32(b + BPB_TotSec32) >= 0x10000)
                && le16(b + BPB_FATSz16) != 0)
            return 0;
    }
    return sign == 0xAA55 ? 2 : 3;
}

static bool test_gpt_header(const uint8_t *h) {
    if (memcmp(h, "EFI PART" "\0\0\1", 12))     // signature and version 1.0
        return false;
    uint32_t hlen = le32(h + GPTH_Size);
    if (hlen < 92 || hlen > SECTOR)
        return false;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < hlen; i++)
        crc = crc32_add(crc, (i >= GPTH_Bcc && i < GPTH_Bcc + 4) ? 0 : h[i]);
    return ~crc == le32(h + GPTH_Bcc) && le32(h + GPTH_PteSize) == SZ_GPTE
        && le32(h + GPTH_PtNum) <= 128;
}

static int fail_read(fsck_t *f) {
    snprintf(f->why, sizeof(f->why), "read error at sector %" PRIu64, f->err_lba);
    refuse(f, f->why);
    return R_OK;
}

static void found(fsck_t *f, uint64_t lba, unsigned fmt) {
    f->vol_lba = lba;
    f->vf_fmt = fmt;
    f->state = ST_BPB;                          // the VBR is in aux
}

static int st_find0(fsck_t *f) {
    int r = rd(f, 0, f->aux);
    if (r == R_YIELD)
        return r;
    if (r == R_FAIL)
        return fail_read(f);
    unsigned fmt = check_fs(f->aux);
    if (fmt <= 1) {
        found(f, 0, fmt);                       // no partition table
    } else if (fmt == 3) {
        refuse(f, "no FAT32 or exFAT volume");
    } else if (f->aux[MBR_Table + 4] == 0xEE) {
        f->state = ST_GPT_HEADER;               // a protective MBR
    } else {
        // The type bytes are ignored, as FatFs ignores them.
        for (unsigned i = 0; i < 4; i++)
            f->mbr_pt[i] = le32(f->aux + MBR_Table + i * 16 + 8);
        f->vf_i = 0;
        f->vf_fmt = 3;
        f->state = ST_MBR;
    }
    return R_OK;
}

static int st_gpt_header(fsck_t *f) {
    int r = rd(f, 1, f->aux);
    if (r == R_YIELD)
        return r;
    if (r == R_FAIL)
        return fail_read(f);
    if (!test_gpt_header(f->aux)) {
        refuse(f, "no FAT32 or exFAT volume");
        return R_OK;
    }
    f->gpt_n = le32(f->aux + GPTH_PtNum);
    f->gpt_lba = le64(f->aux + GPTH_PtOfs);
    f->gpt_want = le32(f->aux + GPTH_PtBcc);
    f->gpt_crc = 0xFFFFFFFFu;
    f->vf_i = 0;
    f->state = ST_GPT_CRC;
    return R_OK;
}

// The entry array's CRC, which FatFs doesn't check: a mismatch is reported, then the check
// goes on.
static int st_gpt_crc(fsck_t *f) {
    uint32_t bytes = f->gpt_n * SZ_GPTE, nsec = (bytes + SECTOR - 1) / SECTOR;
    while (f->vf_i < nsec) {
        int r = rd(f, f->gpt_lba + f->vf_i, f->dirbuf);
        if (r == R_YIELD)
            return r;
        if (r == R_FAIL)
            return fail_read(f);
        uint32_t n = bytes - f->vf_i * SECTOR;
        if (n > SECTOR)
            n = SECTOR;
        for (uint32_t i = 0; i < n; i++)
            f->gpt_crc = crc32_add(f->gpt_crc, f->dirbuf[i]);
        f->vf_i++;
    }
    if (~f->gpt_crc != f->gpt_want)
        emit(f, false, "GPT: the partition entry array's CRC is wrong");
    f->dir_lba = NO_LBA;
    f->vf_i = 0;
    f->state = ST_GPT_SCAN;
    return R_OK;
}

// The first Microsoft basic data partition whose first sector is a VBR.
static int st_gpt_scan(fsck_t *f) {
    while (f->vf_i < f->gpt_n) {
        uint64_t lba = f->gpt_lba + f->vf_i * SZ_GPTE / SECTOR;
        uint32_t ofs = f->vf_i * SZ_GPTE % SECTOR;
        if (f->dir_lba != lba) {
            int r = rd(f, lba, f->dirbuf);
            if (r == R_YIELD)
                return r;
            if (r == R_FAIL)
                return fail_read(f);
            f->dir_lba = lba;
        }
        if (!memcmp(f->dirbuf + ofs, guid_ms_basic, 16)) {
            uint64_t first = le64(f->dirbuf + ofs + GPTE_FstLba);
            int r = rd(f, first, f->aux);
            if (r == R_YIELD)
                return r;
            unsigned fmt = r == R_FAIL ? 4 : check_fs(f->aux);
            if (fmt <= 1) {
                found(f, first, fmt);
                return R_OK;
            }
        }
        f->vf_i++;
    }
    refuse(f, "no FAT32 or exFAT volume");
    return R_OK;
}

// The four entries' start sectors in order, skipping 0; the last one tried decides between
// "no volume" and a read error, as in FatFs.
static int st_mbr(fsck_t *f) {
    while (f->vf_i < 4) {
        uint32_t lba = f->mbr_pt[f->vf_i];
        if (lba) {
            int r = rd(f, lba, f->aux);
            if (r == R_YIELD)
                return r;
            f->vf_fmt = r == R_FAIL ? 4 : check_fs(f->aux);
            if (f->vf_fmt <= 1) {
                found(f, lba, f->vf_fmt);
                return R_OK;
            }
        } else {
            f->vf_fmt = 3;
        }
        f->vf_i++;
    }
    if (f->vf_fmt == 4)
        return fail_read(f);
    refuse(f, "no FAT32 or exFAT volume");
    return R_OK;
}

// The boot sector (in aux), refused for what mount_volume() refuses, in its order.
static int st_bpb(fsck_t *f) {
    const uint8_t *b = f->aux;
    f->res.volume_lba = f->vol_lba;
    if (f->vf_fmt == 1) {
        f->res.kind = FSCK_KIND_EXFAT;
        refuse(f, "exFAT is not checked yet");
        return R_OK;
    }
    if (le16(b + BPB_BytsPerSec) != SECTOR) {
        refuse(f, "bytes per sector not 512");
        return R_OK;
    }
    uint32_t fasize = le16(b + BPB_FATSz16);
    if (!fasize)
        fasize = le32(b + BPB_FATSz32);
    uint8_t nfats = b[BPB_NumFATs], csize = b[BPB_SecPerClus];
    uint16_t nroot = le16(b + BPB_RootEntCnt), nrsv = le16(b + BPB_RsvdSecCnt);
    uint32_t tsect = le16(b + BPB_TotSec16);
    if (!tsect)
        tsect = le32(b + BPB_TotSec32);
    if (nfats != 1 && nfats != 2) {
        refuse(f, "number of FATs not 1 or 2");
        return R_OK;
    }
    if (!is_pow2(csize)) {
        refuse(f, "sectors per cluster 0 or not a power of two");
        return R_OK;
    }
    if (nroot % ENTRIES_PER_SEC) {
        refuse(f, "root directory entries not a whole sector");
        return R_OK;
    }
    if (!nrsv) {
        refuse(f, "reserved sectors 0");
        return R_OK;
    }
    uint64_t sysect = nrsv + (uint64_t)fasize * nfats + nroot / ENTRIES_PER_SEC;
    if (tsect < sysect) {
        refuse(f, "volume smaller than its reserved sectors and FATs");
        return R_OK;
    }
    uint64_t nclst = (tsect - sysect) / csize;
    if (!nclst) {
        refuse(f, "cluster count 0");
        return R_OK;
    }
    if (nclst <= MAX_FAT16) {
        // FAT12 or FAT16: FatFs would mount it, but SOS never makes one.
        refuse(f, "not FAT32 or exFAT");
        return R_OK;
    }
    if (nclst > MAX_FAT32) {
        refuse(f, "too many clusters for FAT32");
        return R_OK;
    }
    if (le16(b + BPB_FSVer32) != 0) {
        refuse(f, "FAT32 version not 0.0");
        return R_OK;
    }
    if (nroot) {
        refuse(f, "root directory entries set on FAT32");
        return R_OK;
    }
    if (fasize < ((nclst + 2) * 4 + SECTOR - 1) / SECTOR) {
        refuse(f, "FAT too small for the cluster count");
        return R_OK;
    }
    // Not mount_volume()'s, but the checker can't read what isn't there.
    if (f->vol_lba + tsect > f->io.sectors) {
        refuse(f, "volume larger than the device");
        return R_OK;
    }
    f->count = (uint32_t)nclst;
    f->root = le32(b + BPB_RootClus32);
    if (f->root < 2 || f->root > f->count + 1) {
        refuse(f, "root directory cluster out of range");
        return R_OK;
    }
    f->nfats = nfats;
    f->csize = csize;
    f->cbytes = csize * SECTOR;
    f->epc = csize * ENTRIES_PER_SEC;
    f->fsize = fasize;
    f->nrsv = nrsv;
    f->tsect = tsect;
    f->media = b[BPB_Media];
    f->fsinfo = le16(b + BPB_FSInfo32);
    f->bkboot = le16(b + BPB_BkBootSec32);
    f->fat_lba = f->vol_lba + nrsv;
    f->data_lba = f->vol_lba + sysect;
    f->dir_expected = DIR_ENTRIES_MAX * DIRENT / f->cbytes;
    f->steps_max = 4 * f->count + 64;
    f->res.kind = FSCK_KIND_FAT32;
    f->res.volume_sectors = tsect;
    f->res.clusters = f->count;
    f->res.cluster_bytes = f->cbytes;
    f->res.passes = (f->count - 1) / f->win_bits + 1;
    fsck_fix_volume(f->fix, f->fat_lba, f->fsize, f->nfats, f->nrsv, &f->res.passes);
    f->state = ST_BACKUP;
    return R_OK;
}

// --- pass 1's boot region checks -------------------------------------------------------------

// The backup boot sector against the boot sector, still in aux.
static int st_backup(fsck_t *f) {
    bool fsinfo_there = f->fsinfo == 1;
    if (f->bkboot && f->bkboot < f->nrsv && !(fsinfo_there && f->bkboot == 1)) {
        int r = rd(f, f->vol_lba + f->bkboot, f->dirbuf);
        if (r != R_OK)
            return r;
        if (memcmp(f->dirbuf, f->aux, SECTOR))
            emit(f, false, "the backup boot sector (sector %u) differs from the boot sector",
                 f->bkboot);
    }
    f->state = ST_FSINFO;
    return R_OK;
}

static int st_fsinfo(fsck_t *f) {
    if (f->fsinfo == 1) {
        int r = rd(f, f->vol_lba + 1, f->dirbuf);
        if (r != R_OK)
            return r;
        if (le16(f->dirbuf + BS_55AA) == 0xAA55 && le32(f->dirbuf + FSI_LeadSig) == 0x41615252
                && le32(f->dirbuf + FSI_StrucSig) == 0x61417272) {
            f->fsi_valid = true;
            f->fsi_free = le32(f->dirbuf + FSI_Free_Count);
            if (fixed(f, fsck_fix_fsinfo(f->fix, f->vol_lba + 1, f->dirbuf)) != R_OK)
                return R_FAIL;
        } else {
            emit(f, false, "the FSInfo sector's signatures are wrong");
        }
    }
    f->state = ST_FAT01;
    return R_OK;
}

// FAT entry 0 holds the media byte; entry 1's top bits say whether the volume was unmounted
// cleanly and whether it had a disk error. FatFs never clears either bit, so on a card only
// SOS writes they only show what a PC or camera did: notes, not problems.
static int st_fat01(fsck_t *f) {
    uint32_t v;
    int r = fat_get(f, 0, &v);
    if (r != R_OK)
        return r;
    uint32_t e0 = le32(f->fatbuf), e1 = le32(f->fatbuf + 4);
    if ((e0 & 0xFF) != f->media)
        emit(f, false, "FAT entry 0 doesn't match the media byte (%02X, boot sector %02X)",
             (unsigned)(e0 & 0xFF), f->media);
    if (!(e1 & 0x08000000u)) {
        f->res.dirty = true;
        emit(f, true, "the volume is flagged dirty (not unmounted cleanly)");
    }
    if (!(e1 & 0x04000000u))
        emit(f, true, "the volume is flagged as having had a disk error");
    f->vf_i = 0;
    f->sub = 0;
    f->diff_n = 0;
    f->state = f->nfats == 2 ? ST_FATCMP : ST_PASS;
    return R_OK;
}

// The two FATs, sector by sector.
static int st_fatcmp(fsck_t *f) {
    while (f->vf_i < f->fsize) {
        if (!f->sub) {
            int r = rd(f, f->fat_lba + f->vf_i, f->aux);
            if (r != R_OK)
                return r;
            f->sub = 1;
        }
        int r = rd(f, f->fat_lba + f->fsize + f->vf_i, f->dirbuf);
        if (r != R_OK)
            return r;
        if (memcmp(f->aux, f->dirbuf, SECTOR) && !f->diff_n++)
            f->diff_first = f->vf_i;
        if (fixed(f, fsck_fix_fat_copy(f->fix, f->vf_i, f->aux, f->dirbuf)) != R_OK)
            return R_FAIL;
        f->sub = 0;
        f->vf_i++;
    }
    if (f->diff_n)
        emit(f, false, "the two FATs differ in %" PRIu32 " %s (first: FAT sector %" PRIu32 ")",
             f->diff_n, plural(f->diff_n, "sector", "sectors"), f->diff_first);
    f->state = ST_PASS;
    return R_OK;
}

// --- chains ---------------------------------------------------------------------------------

static void chain_start(fsck_t *f, uint32_t start, uint32_t expected) {
    chain_t *ch = &f->ch;
    memset(ch, 0, sizeof(*ch));
    ch->start = ch->cur = start;
    ch->expected = expected;
    if (start < 2 || start > f->count + 1) {
        ch->status = CH_START_RANGE;
        ch->bad = start;
    }
}

// Follow the chain to its end, marking what it reaches in the window.
static int chain_run(fsck_t *f) {
    chain_t *ch = &f->ch;
    while (ch->status == CH_RUN) {
        if (f->steps >= f->steps_max) {
            ch->status = CH_STOPPED;
            break;
        }
        uint32_t c = ch->cur, v;
        int r = fat_get(f, c, &v);
        if (r != R_OK)
            return r;
        f->steps++;
        if (v == 0 || v == FAT32_BAD) {         // c isn't in use: the chain is broken before it
            ch->status = v ? CH_BAD : CH_FREE;
            ch->bad = c;
            break;
        }
        ch->n++;
        ch->last = c;
        if (ch->n == ch->expected)
            ch->keep = c;
        ch->last_in = in_window(f, c);
        ch->last_hit = ch->last_in && mark(f, c);
        if (ch->last_hit) {
            ch->hits++;
            f->res.cross_linked++;
        }
        if (v >= FAT32_EOC) {
            ch->status = CH_EOC;
            break;
        }
        if (v < 2 || v > f->count + 1) {
            ch->status = CH_RANGE;
            ch->bad = v;
            break;
        }
        if (ch->n > f->count) {
            ch->status = CH_LOOP;
            break;
        }
        if (!ch->brent) {
            if (ch->n >= ch->expected) {
                ch->brent = true;
                ch->tort = v;
                ch->power = 1;
                ch->lam = 0;
            }
        } else {
            ch->lam++;
            if (v == ch->tort) {
                ch->status = CH_LOOP;
                break;
            }
            if (ch->lam == ch->power) {
                ch->tort = v;
                ch->power <<= 1;
                ch->lam = 0;
            }
        }
        ch->cur = v;
    }
    if (ch->status == CH_LOOP) {
        // A loop's second visits marked clusters it had marked itself: not cross-links.
        f->res.cross_linked -= ch->hits;
        ch->hits = 0;
    }
    return R_OK;
}

// Pass 1: how the chain broke, if it did.
static void report_chain(fsck_t *f) {
    const chain_t *ch = &f->ch;
    switch (ch->status) {
    case CH_START_RANGE:
        emit(f, false, "%s: bad start cluster %" PRIu32, path(f), ch->bad);
        break;
    case CH_FREE:
        emit(f, false, "%s: chain reaches free cluster %" PRIu32, path(f), ch->bad);
        break;
    case CH_BAD:
        emit(f, false, "%s: chain reaches bad cluster %" PRIu32, path(f), ch->bad);
        break;
    case CH_RANGE:
        emit(f, false, "%s: bad cluster number %" PRIu32 " after cluster %" PRIu32, path(f),
             ch->bad, ch->last);
        break;
    case CH_LOOP:
        emit(f, false, "%s: chain loops", path(f));
        break;
    }
}

// Any pass: chains that share a cluster share their last one (one successor per FAT entry), so
// an entry is cross-linked with an earlier one exactly when its last cluster was already
// marked. Reported in the pass whose window holds that cluster: once, whatever the window.
static void report_cross_link(fsck_t *f) {
    const chain_t *ch = &f->ch;
    if (ch->status != CH_LOOP && ch->n && ch->last_in && ch->last_hit)
        emit(f, false, "%s: cross-linked with an earlier entry (both end at cluster %" PRIu32 ")",
             path(f), ch->last);
}

// Pass 1: a file's size against its chain (a chain that ended properly).
static void report_size(fsck_t *f, uint32_t size, uint32_t n) {
    uint32_t need = size ? (size - 1) / f->cbytes + 1 : 0;
    f->res.long_chains += n > need;
    if (need != n)
        emit(f, n > need, "%s: %" PRIu32 " %s for %" PRIu32 " bytes, chain has %" PRIu32, path(f),
             need, plural(need, "cluster", "clusters"), size, n);
}

// --- directory entries and names ------------------------------------------------------------

// Case folding: the blocks of FatFs's ff_wtoupper() table (ffunicode.c) for the ranges in
// fsck.h. A block is `count` code units from `base`, shifted by `shift`, or in case pairs
// (even upper, odd lower, counted from base) when `shift` is 0.
typedef struct {
    uint16_t base;
    uint8_t count;
    int8_t shift;
} fold_block_t;

static const fold_block_t fold_blocks[] = {
    {0x0061, 26, -32}, {0x00E0, 23, -32}, {0x00F8, 7, -32},
    {0x0100, 48, 0}, {0x0132, 6, 0}, {0x0139, 16, 0}, {0x014A, 46, 0}, {0x0179, 6, 0},
    {0x03B1, 17, -32}, {0x03C4, 8, -32}, {0x03D8, 24, 0},
    {0x0430, 32, -32}, {0x0450, 16, -80}, {0x0460, 34, 0}, {0x048A, 54, 0}, {0x04C1, 14, 0},
    {0x04D0, 68, 0},
};

static const uint16_t fold_single[][2] = {
    {0x00FF, 0x0178}, {0x037B, 0x03FD}, {0x037C, 0x03FE}, {0x037D, 0x03FF},
    {0x03AC, 0x0386}, {0x03AD, 0x0388}, {0x03AE, 0x0389}, {0x03AF, 0x038A},
    {0x03C2, 0x03A3}, {0x03C3, 0x03A3}, {0x03CC, 0x038C}, {0x03CD, 0x038E}, {0x03CE, 0x038F},
    {0x03F2, 0x03F9}, {0x03F8, 0x03F7}, {0x03FB, 0x03FA}, {0x04CF, 0x04C0},
};

uint16_t fsck_upcase(uint16_t c) {
    for (size_t i = 0; i < sizeof(fold_blocks) / sizeof(fold_blocks[0]); i++) {
        const fold_block_t *b = &fold_blocks[i];
        if (c >= b->base && c < b->base + b->count) {
            if (b->shift)
                return (uint16_t)(c + b->shift);
            return (uint16_t)(c - ((c - b->base) & 1));
        }
    }
    for (size_t i = 0; i < sizeof(fold_single) / sizeof(fold_single[0]); i++)
        if (fold_single[i][0] == c)
            return fold_single[i][1];
    return c;
}

static uint32_t entry_cluster(const uint8_t *e) {
    return (uint32_t)le16(e + DIR_FstClusHI) << 16 | le16(e + DIR_FstClusLO);
}

// ff.c's sum_sfn(): the checksum long-name entries carry of their short name.
static uint8_t sum_sfn(const uint8_t *e) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)((sum >> 1) + (sum << 7) + e[i]);
    return sum;
}

// ff.c's pick_lfn(): one long-name entry's characters into `buf`, false if FatFs rejects it.
static bool pick_lfn(uint16_t *buf, const uint8_t *e) {
    static const uint8_t ofs[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    if (le16(e + DIR_FstClusLO) != 0)
        return false;
    uint32_t i = ((uint32_t)(e[0] & ~LLEF) - 1u) * 13u;
    uint16_t wc = 1;
    for (int s = 0; s < 13; s++) {
        uint16_t uc = le16(e + ofs[s]);
        if (wc) {
            if (i >= NAME_UNITS)
                return false;
            buf[i++] = wc = uc;
        } else if (uc != 0xFFFF) {
            return false;
        }
    }
    if ((e[0] & LLEF) && wc) {
        if (i >= NAME_UNITS)
            return false;
        buf[i] = 0;
    }
    return true;
}

// Orphaned long-name entries of the scan, merged into runs of neighbours before reporting.
static void orphan_flush(fsck_t *f) {
    if (!f->orph_len)
        return;
    if (f->orph_len == 1)
        emit(f, false, "%s: orphaned long-name entry at entry %" PRIu32, path(f), f->orph_start);
    else
        emit(f, false, "%s: %" PRIu32 " orphaned long-name entries from entry %" PRIu32, path(f),
             f->orph_len, f->orph_start);
    f->orph_len = 0;
}

static void orphan_add(fsck_t *f, bool report, uint32_t start, uint32_t n) {
    if (!report || !n)
        return;
    if (f->orph_len && f->orph_start + f->orph_len == start) {
        f->orph_len += n;
        return;
    }
    orphan_flush(f);
    f->orph_start = start;
    f->orph_len = n;
}

static void lfn_reset(lfn_t *l) {
    l->ord = l->sum = 0xFF;
    l->plen = 0;
}

static void lfn_drop(fsck_t *f, lfn_t *l, bool report) {
    orphan_add(f, report, l->pstart, l->plen);
    l->plen = 0;
}

// One entry (not the end mark) through ff.c's dir_read() rules for long names. K_ITEM: a short
// entry FatFs lists, with *attached telling whether the long name in `buf` is its own. With
// `report`, long-name entries FatFs would discard are collected as orphans.
static int lfn_feed(fsck_t *f, lfn_t *l, uint16_t *buf, const uint8_t *e, uint32_t idx,
                    bool report, bool *attached) {
    uint8_t c = e[0], a = e[DIR_Attr] & 0x3F;
    *attached = false;
    if (c == DDEM || c == '.' || (a & ~AM_ARC) == AM_VOL) {
        lfn_drop(f, l, report);
        l->ord = 0xFF;
        if (c == '.' && a == AM_LFN) {          // a long-name entry FatFs can't use
            orphan_add(f, report, idx, 1);
            return K_SKIP;
        }
        return c == '.' ? K_DOT : K_SKIP;
    }
    if (a == AM_LFN) {
        if (c & LLEF) {                         // the first entry of a name (its last part)
            lfn_drop(f, l, report);
            l->sum = e[LDIR_Chksum];
            c &= (uint8_t)~LLEF;
            l->ord = c;
            l->pstart = idx;
        }
        if (c == l->ord && l->sum == e[LDIR_Chksum] && pick_lfn(buf, e)) {
            l->ord--;
            l->plen++;
        } else {
            lfn_drop(f, l, report);
            orphan_add(f, report, idx, 1);
            l->ord = 0xFF;
        }
        return K_LFN;
    }
    *attached = l->plen && l->ord == 0 && l->sum == sum_sfn(e);
    if (!*attached)
        lfn_drop(f, l, report);
    l->plen = 0;
    l->ord = 0xFF;
    return K_ITEM;
}

// The short name as FatFs shows it ("README.TXT", "readme.txt" with the NT case bits), into
// `out` (13 bytes); its length.
static size_t sfn_name(const uint8_t *e, uint8_t *out) {
    size_t n = 0;
    int base = 8, ext = 3;
    while (base && e[base - 1] == ' ')
        base--;
    while (ext && e[8 + ext - 1] == ' ')
        ext--;
    for (int i = 0; i < base; i++) {
        uint8_t c = (i == 0 && e[0] == RDDEM) ? DDEM : e[i];
        if ((e[DIR_NTres] & 0x08) && c >= 'A' && c <= 'Z')
            c = (uint8_t)(c + 32);
        out[n++] = c;
    }
    if (ext)
        out[n++] = '.';
    for (int i = 0; i < ext; i++) {
        uint8_t c = e[8 + i];
        if ((e[DIR_NTres] & 0x10) && c >= 'A' && c <= 'Z')
            c = (uint8_t)(c + 32);
        out[n++] = c;
    }
    return n;
}

static bool lfn_name(const uint16_t *buf, bool attached) {
    return attached && buf[0];
}

// One code point onto the path as UTF-8; false (and "..." added) when the path is full.
static bool path_put(fsck_t *f, uint32_t cp) {
    if (cp < 0x20 || cp == 0x7F || (cp >= 0xD800 && cp < 0xE000))
        cp = '?';                               // shown, not trusted
    uint8_t u[4];
    unsigned n;
    if (cp < 0x80) {
        u[0] = (uint8_t)cp;
        n = 1;
    } else if (cp < 0x800) {
        u[0] = (uint8_t)(0xC0 | cp >> 6);
        u[1] = (uint8_t)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        u[0] = (uint8_t)(0xE0 | cp >> 12);
        u[1] = (uint8_t)(0x80 | (cp >> 6 & 0x3F));
        u[2] = (uint8_t)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        u[0] = (uint8_t)(0xF0 | cp >> 18);
        u[1] = (uint8_t)(0x80 | (cp >> 12 & 0x3F));
        u[2] = (uint8_t)(0x80 | (cp >> 6 & 0x3F));
        u[3] = (uint8_t)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (f->path_len + n + 4 > PATH_LEN) {       // room kept for "..." and the terminator
        if (f->path_len < 3 || memcmp(f->path + f->path_len - 3, "...", 3)) {
            memcpy(f->path + f->path_len, "...", 3);
            f->path_len = (uint16_t)(f->path_len + 3);
        }
        f->path[f->path_len] = 0;
        return false;
    }
    memcpy(f->path + f->path_len, u, n);
    f->path_len = (uint16_t)(f->path_len + n);
    f->path[f->path_len] = 0;
    return true;
}

// "/<name>" onto the path: the long name when the entry has one, else the short name.
static void path_add(fsck_t *f, const uint16_t *buf, bool attached, const uint8_t *e) {
    if (!path_put(f, '/'))
        return;
    if (lfn_name(buf, attached)) {
        for (uint32_t i = 0; i < NAME_UNITS && buf[i]; i++) {
            uint32_t cp = buf[i];
            if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < NAME_UNITS && buf[i + 1] >= 0xDC00
                    && buf[i + 1] < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (buf[i + 1] - 0xDC00u);
                i++;
            }
            if (!path_put(f, cp))
                return;
        }
        return;
    }
    uint8_t s[13];
    size_t n = sfn_name(e, s);
    for (size_t i = 0; i < n; i++)
        if (!path_put(f, s[i] < 0x80 ? s[i] : '?'))
            return;
}

static void path_cut(fsck_t *f, uint16_t len) {
    f->path_len = len;
    f->path[len] = 0;
}

// Names FatFs refuses (fsck.h).
static bool name_bad(const uint16_t *buf, bool attached, const uint8_t *e) {
    if (lfn_name(buf, attached))
        for (uint32_t i = 0; i < NAME_UNITS && buf[i]; i++)
            if (buf[i] < 0x20 || buf[i] == 0x7F || (buf[i] < 0x80 && strchr("\"*/:<>?\\|", buf[i])))
                return true;
    if (e[0] == ' ')
        return true;
    for (int i = 0; i < 11; i++) {
        uint8_t c = e[i];
        if (i == 0 && c == RDDEM)
            continue;
        if (c < 0x20 || c == 0x7F || strchr("\"*+,./:;<=>?[\\]|", c))
            return true;
    }
    return false;
}

// An entry's keys for repeated names: the long name upcased into `buf` (in place, or built
// from the short name), and the 11 bytes of the short name.
static void make_keys(uint16_t *buf, bool attached, const uint8_t *e, uint8_t *sk) {
    if (lfn_name(buf, attached)) {
        for (uint32_t i = 0; i < NAME_UNITS && buf[i]; i++)
            buf[i] = fsck_upcase(buf[i]);
    } else {
        uint8_t s[13];
        size_t n = sfn_name(e, s);
        for (size_t i = 0; i < n; i++)
            buf[i] = fsck_upcase(s[i]);
        buf[n] = 0;
    }
    memcpy(sk, e, 11);
    if (sk[0] == RDDEM)
        sk[0] = DDEM;
}

static bool key_eq(const uint16_t *a, const uint16_t *b) {
    for (uint32_t i = 0; i < NAME_UNITS; i++) {
        if (a[i] != b[i])
            return false;
        if (!a[i])
            return true;
    }
    return true;
}

// Add a key to the Bloom filter (three bits of its FNV-1a hash); true if all were set already.
static bool bloom_add(fsck_t *f, uint8_t domain, const uint16_t *k, const uint8_t *sk) {
    uint32_t h = 2166136261u ^ domain;
    if (k) {
        for (uint32_t i = 0; i < NAME_UNITS && k[i]; i++) {
            h = (h ^ (k[i] & 0xFF)) * 16777619u;
            h = (h ^ (k[i] >> 8)) * 16777619u;
        }
    } else {
        for (int i = 0; i < 11; i++)
            h = (h ^ sk[i]) * 16777619u;
    }
    bool all = true;
    uint32_t bits[3] = {h % BLOOM_BITS, (h >> 12) % BLOOM_BITS, (h >> 20 ^ h << 4) % BLOOM_BITS};
    for (int i = 0; i < 3; i++) {
        uint8_t m = (uint8_t)(1u << (bits[i] & 7));
        all = all && (f->bloom[bits[i] >> 3] & m);
        f->bloom[bits[i] >> 3] |= m;
    }
    return all;
}

// Entry p->ord of the directory whose readable chain is `nclus` clusters, into `buf` (which
// holds sector *buf_lba). R_END past the chain or FatFs's 65536 entries.
static int dir_get(fsck_t *f, uint32_t nclus, dirpos_t *p, uint8_t *buf, uint64_t *buf_lba,
                   const uint8_t **e) {
    if (p->ord >= DIR_ENTRIES_MAX || p->ord / f->epc >= nclus)
        return R_END;
    while (p->ci < p->ord / f->epc) {
        uint32_t v;
        int r = fat_get(f, p->clus, &v);
        if (r != R_OK)
            return r;
        if (v < 2 || v > f->count + 1)
            return R_END;                       // the chain changed since it was followed
        p->clus = v;
        p->ci++;
    }
    uint64_t lba = clus_lba(f, p->clus) + (p->ord % f->epc) / ENTRIES_PER_SEC;
    if (*buf_lba != lba) {
        int r = rd(f, lba, buf);
        if (r != R_OK) {
            if (r == R_FAIL)
                *buf_lba = NO_LBA;
            return r;
        }
        *buf_lba = lba;
    }
    *e = buf + (p->ord % ENTRIES_PER_SEC) * DIRENT;
    return R_OK;
}

// --- the walk -------------------------------------------------------------------------------

static frame_t *top(fsck_t *f) {
    return &f->stack[f->depth - 1];
}

static void stop_walk(fsck_t *f) {
    fsck_fix_unread(f->fix);
    if (f->pass == 1)
        emit(f, false, "stopped walking the directories: chains overlap too much "
             "(the check is incomplete)");
    f->wk = WK_END;
}

// Back to the directory being read, after one of its entries.
static void entry_done(fsck_t *f) {
    path_cut(f, top(f)->path_len);
    f->wk = WK_ENTRY;
}

static void start_dir(fsck_t *f) {
    frame_t *fr = top(f);
    fr->pos = (dirpos_t){fr->start, 0, 0};
    lfn_reset(&f->lf);
    if (f->pass == 1) {
        memset(f->bloom, 0, sizeof(f->bloom));
        f->rescan_max = 4 * (uint64_t)fr->nclus * f->csize + 16;
        f->rescan_used = 0;
        f->dup_off = false;
        f->seen_dot = f->seen_dotdot = false;
        f->orph_len = 0;
        f->wk = WK_SCAN;
    } else {
        f->wk = WK_ENTRY;
    }
}

static int wk_dirchain(fsck_t *f) {
    int r = chain_run(f);
    if (r != R_OK)
        return r;
    const chain_t *ch = &f->ch;
    if (ch->status == CH_STOPPED) {
        stop_walk(f);
        return R_OK;
    }
    if (f->pass == 1)
        report_chain(f);
    report_cross_link(f);
    if (ch->status == CH_LOOP || ch->status == CH_FREE || ch->status == CH_BAD)
        fsck_fix_unread(f->fix);                // FatFs reads entries this walk doesn't
    if (ch->status == CH_LOOP || ch->status == CH_START_RANGE || !ch->n) {
        if (!f->depth)
            f->wk = WK_END;                     // the root can't be read
        else
            entry_done(f);
        return R_OK;
    }
    f->stack[f->depth++] = (frame_t){.start = ch->start, .nclus = ch->n, .path_len = f->path_len};
    start_dir(f);
    return R_OK;
}

// A "." or ".." entry, or another name starting with '.', during the scan.
static void scan_dot(fsck_t *f, const uint8_t *e, uint32_t idx) {
    const frame_t *fr = top(f);
    bool dot = !memcmp(e, ".          ", 11), dotdot = !memcmp(e, "..         ", 11);
    uint32_t cl = entry_cluster(e);
    if (!dot && !dotdot) {
        path_add(f, NULL, false, e);
        emit(f, false, "%s: name has characters FatFs refuses", path(f));
        path_cut(f, fr->path_len);
    } else if (f->depth > 1 && idx == 0 && dot) {
        f->seen_dot = true;
        if (cl != fr->start)
            emit(f, false, "%s: '.' points at cluster %" PRIu32 ", not %" PRIu32, path(f), cl,
                 fr->start);
    } else if (f->depth > 1 && idx == 1 && dotdot) {
        f->seen_dotdot = true;
        uint32_t parent = f->depth == 2 ? 0 : f->stack[f->depth - 2].start;
        if (cl != parent && !(f->depth == 2 && cl == f->root))
            emit(f, false, "%s: '..' points at cluster %" PRIu32 ", not %" PRIu32, path(f), cl,
                 parent);
    } else {
        emit(f, false, "%s: '%s' entry where it doesn't belong (entry %" PRIu32 ")", path(f),
             dot ? "." : "..", idx);
    }
}

static void scan_end(fsck_t *f) {
    orphan_flush(f);
    if (f->depth > 1 && !f->seen_dot)
        emit(f, false, "%s: no '.' entry", path(f));
    if (f->depth > 1 && !f->seen_dotdot)
        emit(f, false, "%s: no '..' entry", path(f));
    frame_t *fr = top(f);
    fr->pos = (dirpos_t){fr->start, 0, 0};
    lfn_reset(&f->lf);
    f->wk = WK_ENTRY;
}

// Pass 1, before a directory's entries are followed: names, long-name sequences, "." and "..",
// and repeated names. Reads the directory once more than the other passes do.
static int wk_scan(fsck_t *f) {
    frame_t *fr = top(f);
    for (;;) {
        const uint8_t *e;
        int r = dir_get(f, fr->nclus, &fr->pos, f->dirbuf, &f->dir_lba, &e);
        if (r == R_END || (r == R_OK && !e[0])) {
            lfn_drop(f, &f->lf, true);
            scan_end(f);
            return R_OK;
        }
        if (r != R_OK)
            return r;
        uint32_t idx = fr->pos.ord;
        bool att;
        int kind = lfn_feed(f, &f->lf, f->lfn, e, idx, true, &att);
        if (kind == K_DOT) {
            scan_dot(f, e, idx);
        } else if (kind == K_ITEM) {
            path_add(f, f->lfn, att, e);
            if (name_bad(f->lfn, att, e))
                emit(f, false, "%s: name has characters FatFs refuses", path(f));
            if (!f->dup_off) {
                make_keys(f->lfn, att, e, f->key_sfn);
                bool l = bloom_add(f, 1, f->lfn, NULL), s = bloom_add(f, 2, NULL, f->key_sfn);
                if (l || s) {                   // maybe seen before: confirm by a rescan
                    f->rpos = (dirpos_t){fr->start, 0, 0};
                    f->rescan_from = f->reads;
                    lfn_reset(&f->rl);
                    f->wk = WK_RESCAN;
                    return R_OK;
                }
            }
            path_cut(f, fr->path_len);
        }
        fr->pos.ord++;
    }
}

static void rescan_done(fsck_t *f) {
    frame_t *fr = top(f);
    f->rescan_used += f->reads - f->rescan_from;
    path_cut(f, fr->path_len);
    fr->pos.ord++;
    f->wk = WK_SCAN;
}

// The directory from its start up to the scan's entry, looking for the entry's name.
static int wk_rescan(fsck_t *f) {
    frame_t *fr = top(f);
    while (f->rpos.ord < fr->pos.ord) {
        const uint8_t *e;
        int r = dir_get(f, fr->nclus, &f->rpos, f->aux, &f->aux_lba, &e);
        if (r == R_END)
            break;
        if (r != R_OK)
            return r;
        // Counted in reads, not calls, so the limit falls in the same place whatever the budget.
        if (f->rescan_used + (f->reads - f->rescan_from) > f->rescan_max) {
            f->dup_off = true;
            path_cut(f, fr->path_len);
            emit(f, true, "%s: too many names to check them all for repeats", path(f));
            break;
        }
        bool att;
        if (e[0] && lfn_feed(f, &f->rl, f->rlfn, e, f->rpos.ord, false, &att) == K_ITEM) {
            uint8_t sk[11];
            make_keys(f->rlfn, att, e, sk);
            if (key_eq(f->rlfn, f->lfn) || !memcmp(sk, f->key_sfn, 11)) {
                emit(f, false, "%s: name repeated in the directory", path(f));
                break;
            }
        }
        f->rpos.ord++;
    }
    rescan_done(f);
    return R_OK;
}

// The stack index of the directory starting at `start` (0, FatFs's root, is the root), or -1.
static int on_stack(const fsck_t *f, uint32_t start) {
    if (!start)
        return 0;
    for (uint32_t i = 0; i < f->depth; i++)
        if (f->stack[i].start == start)
            return (int)i;
    return -1;
}

static void entry_dir(fsck_t *f, uint32_t start) {
    int anc = on_stack(f, start);
    if (anc >= 0) {
        if (f->pass == 1) {
            uint16_t alen = f->stack[anc].path_len;
            emit(f, false, "%s: directory loops back to %.*s", path(f), alen ? (int)alen : 1,
                 alen ? f->path : "/");
        }
        entry_done(f);
        return;
    }
    chain_start(f, start, f->dir_expected);
    if (f->depth >= FSCK_DEPTH_MAX) {
        if (f->pass == 1)
            emit(f, false, "%s: nested more than %d levels deep: not read", path(f),
                 FSCK_DEPTH_MAX - 1);
        fsck_fix_unread(f->fix);
        f->ent_kind = ENT_DEEP_DIR;
        f->wk = WK_FILECHAIN;
    } else {
        f->wk = WK_DIRCHAIN;
    }
}

// Every pass: each entry's chain is followed, and directories are entered.
static int wk_entry(fsck_t *f) {
    frame_t *fr = top(f);
    for (;;) {
        const uint8_t *e;
        int r = dir_get(f, fr->nclus, &fr->pos, f->dirbuf, &f->dir_lba, &e);
        if (r == R_END || (r == R_OK && !e[0])) {
            f->wk = WK_POP;
            return R_OK;
        }
        if (r != R_OK)
            return r;
        bool att;
        int kind = lfn_feed(f, &f->lf, f->lfn, e, fr->pos.ord, false, &att);
        fr->pos.ord++;
        if (kind != K_ITEM)
            continue;
        path_add(f, f->lfn, att, e);
        uint32_t start = entry_cluster(e), size = le32(e + DIR_FileSize);
        if (e[DIR_Attr] & AM_DIR) {
            if (f->pass == 1)
                f->res.dirs++;
            entry_dir(f, start);                // FAT32 directories have no size to check
            return R_OK;
        }
        if (f->pass == 1)
            f->res.files++;
        if (!start) {
            if (f->pass == 1)
                report_size(f, size, 0);
            path_cut(f, fr->path_len);
            continue;
        }
        f->ent_kind = ENT_FILE;
        f->ent_size = size;
        chain_start(f, start, size ? (size - 1) / f->cbytes + 1 : 0);
        f->wk = WK_FILECHAIN;
        return R_OK;
    }
}

// Repair, pass 1: a file's chain longer than the file gets an end mark at the last cluster the
// file needs (fsck_repair.h).
static int fix_trim(fsck_t *f) {
    uint32_t need = f->ent_size ? (f->ent_size - 1) / f->cbytes + 1 : 0, v;
    int rc = fsck_fix_trim(f->fix, need, f->ch.n);
    if (rc != FSCK_FIX_LOAD)
        return fixed(f, rc);
    int r = fat_get(f, f->ch.keep, &v);
    if (r != R_OK)
        return r;
    return fixed(f, fsck_fix_end_mark(f->fix, f->fatbuf, f->fat_buf_lba, f->ch.keep));
}

static int wk_filechain(fsck_t *f) {
    int r = chain_run(f);
    if (r != R_OK)
        return r;
    if (f->ch.status == CH_STOPPED) {
        stop_walk(f);
        return R_OK;
    }
    if (f->pass == 1 && f->ent_kind == ENT_FILE && f->ch.status == CH_EOC
            && (r = fix_trim(f)) != R_OK)
        return r;
    if (f->pass == 1) {
        report_chain(f);
        if (f->ent_kind == ENT_FILE && f->ch.status == CH_EOC)
            report_size(f, f->ent_size, f->ch.n);
    }
    report_cross_link(f);
    entry_done(f);
    return R_OK;
}

// The walk of one pass. R_END when it is over.
static int walk(fsck_t *f) {
    for (;;) {
        int r = R_OK;
        switch (f->wk) {
        case WK_ROOT:
            path_cut(f, 0);
            f->depth = 0;
            if (f->pass == 1)
                f->res.dirs++;
            chain_start(f, f->root, f->dir_expected);
            f->wk = WK_DIRCHAIN;
            break;
        case WK_DIRCHAIN:
            r = wk_dirchain(f);
            break;
        case WK_SCAN:
            r = wk_scan(f);
            break;
        case WK_RESCAN:
            r = wk_rescan(f);
            break;
        case WK_ENTRY:
            r = wk_entry(f);
            break;
        case WK_FILECHAIN:
            r = wk_filechain(f);
            break;
        case WK_POP:
            f->depth--;
            if (!f->depth) {
                f->wk = WK_END;
                break;
            }
            lfn_reset(&f->lf);
            entry_done(f);
            break;
        case WK_END:
            return R_END;
        }
        if (r != R_OK)
            return r;
    }
}

// --- passes ---------------------------------------------------------------------------------

static void st_pass(fsck_t *f) {
    f->pass++;
    f->res.pass = f->pass;
    uint64_t lo = 2 + (uint64_t)(f->pass - 1) * f->win_bits, hi = lo + f->win_bits;
    if (hi > (uint64_t)f->count + 2)
        hi = (uint64_t)f->count + 2;
    f->win_lo = (uint32_t)lo;
    f->win_hi = (uint32_t)hi;
    memset(f->win, 0, (f->win_hi - f->win_lo + 7) / 8);
    f->steps = 0;
    f->wk = WK_ROOT;
    f->dir_lba = f->aux_lba = NO_LBA;
    f->state = ST_WALK;
}

static void lost_flush(fsck_t *f) {
    if (!f->run_len)
        return;
    char size[40];
    fmt_size(size, sizeof(size), (uint64_t)f->run_len * f->cbytes);
    f->res.lost_clusters += f->run_len;
    if (f->run_len == 1)
        emit(f, false, "lost: cluster %" PRIu32 " (%s)", f->run_start, size);
    else
        emit(f, false, "lost: clusters %" PRIu32 "-%" PRIu32 " (%s)", f->run_start,
             f->run_start + f->run_len - 1, size);
    f->run_len = 0;
}

// The window against the FAT: used but not reached is lost. A run of lost clusters can go on
// into the next window, so it is kept until it ends.
static int st_compare(fsck_t *f) {
    while (f->cmp_next < f->win_hi) {
        uint32_t c = f->cmp_next, v;
        int r = fixed(f, fsck_fix_flush(f->fix, f->fatbuf, f->fat_lba + c / FAT_PER_SEC));
        if (r != R_OK)
            return r;
        r = fat_get(f, c, &v);
        if (r != R_OK)
            return r;
        if (v == 0) {
            f->res.free_clusters++;
            lost_flush(f);
        } else if (v == FAT32_BAD) {
            f->res.bad_clusters++;              // never lost, never to be freed
            lost_flush(f);
        } else if (marked(f, c)) {
            lost_flush(f);
        } else if (f->run_len && f->run_start + f->run_len == c) {
            f->run_len++;
            fsck_fix_lost(f->fix, f->fatbuf, f->fat_buf_lba, c);
        } else {
            lost_flush(f);
            f->run_start = c;
            f->run_len = 1;
            fsck_fix_lost(f->fix, f->fatbuf, f->fat_buf_lba, c);
        }
        f->cmp_next++;
    }
    int r = fixed(f, fsck_fix_flush(f->fix, f->fatbuf, NO_LBA));
    if (r != R_OK)
        return r;
    f->state = f->pass < f->res.passes ? ST_PASS : ST_FINISH;
    return R_OK;
}

static void st_finish(fsck_t *f) {
    lost_flush(f);
    fsck_result_t *res = &f->res;
    if (f->fsi_valid && f->fsi_free != 0xFFFFFFFFu && f->fsi_free != res->free_clusters)
        emit(f, true, "the FSInfo free count is %" PRIu32 ", counted %" PRIu32, f->fsi_free,
             res->free_clusters);
    size_t n;
    if (res->problems)
        n = (size_t)snprintf(res->summary, sizeof(res->summary), "%" PRIu32 " %s", res->problems,
                             plural(res->problems, "problem", "problems"));
    else
        n = (size_t)snprintf(res->summary, sizeof(res->summary), "no problems");
    if (res->problems && n < sizeof(res->summary)) {
        char size[40];
        fmt_size(size, sizeof(size), (uint64_t)res->lost_clusters * f->cbytes);
        n += (size_t)snprintf(res->summary + n, sizeof(res->summary) - n,
                              "; %s lost, %" PRIu32 " cross-linked", size, res->cross_linked);
    }
    if (res->bad_clusters && n < sizeof(res->summary))
        n += (size_t)snprintf(res->summary + n, sizeof(res->summary) - n, "; %" PRIu32 " bad %s",
                              res->bad_clusters, plural(res->bad_clusters, "cluster", "clusters"));
    if (res->dirty && n < sizeof(res->summary))
        snprintf(res->summary + n, sizeof(res->summary) - n, "; volume flagged dirty");
    f->state = ST_DONE;
}

// --- the interface --------------------------------------------------------------------------

size_t fsck_state_size(void) {
    return (sizeof(fsck_t) + 7) & ~(size_t)7;
}

fsck_t *fsck_new(const fsck_io_t *io, void *mem, size_t mem_len) {
    if (!io || !io->read || !io->sectors || !mem)
        return NULL;
    size_t pad = (8 - ((uintptr_t)mem & 7)) & 7;
    if (mem_len < pad + fsck_state_size() + FSCK_WINDOW_MIN)
        return NULL;
    fsck_t *f = (fsck_t *)(void *)((uint8_t *)mem + pad);
    memset(f, 0, sizeof(*f));
    f->io = *io;
    f->win = (uint8_t *)f + fsck_state_size();
    size_t win = mem_len - pad - fsck_state_size();
    if (win > 0x10000000)                       // 2^31 clusters: more than FAT32 can have
        win = 0x10000000;
    f->win_bits = (uint32_t)(win * 8);
    f->fat_buf_lba = f->dir_lba = f->aux_lba = NO_LBA;
    f->state = ST_FIND0;
    return f;
}

int fsck_step(fsck_t *f, uint32_t budget) {
    f->budget = budget ? budget : 1;
    for (;;) {
        int r = R_OK;
        switch (f->state) {
        case ST_FIND0:      r = st_find0(f); break;
        case ST_GPT_HEADER: r = st_gpt_header(f); break;
        case ST_GPT_CRC:    r = st_gpt_crc(f); break;
        case ST_GPT_SCAN:   r = st_gpt_scan(f); break;
        case ST_MBR:        r = st_mbr(f); break;
        case ST_BPB:        r = st_bpb(f); break;
        case ST_BACKUP:     r = st_backup(f); break;
        case ST_FSINFO:     r = st_fsinfo(f); break;
        case ST_FAT01:      r = st_fat01(f); break;
        case ST_FATCMP:     r = st_fatcmp(f); break;
        case ST_PASS:       st_pass(f); break;
        case ST_WALK:
            r = walk(f);
            if (r == R_END) {
                f->cmp_next = f->win_lo;
                f->state = ST_COMPARE;
                r = R_OK;
            }
            break;
        case ST_COMPARE:    r = st_compare(f); break;
        case ST_FINISH:     st_finish(f); break;
        case ST_DONE:       return FSCK_DONE;
        case ST_FAILED:     return FSCK_FAILED;
        }
        if (r == R_YIELD)
            return FSCK_MORE;
        if (r == R_FAIL && f->state != ST_FAILED)
            fail_read(f);
    }
}

void fsck_set_fix(fsck_t *f, fsck_fix_t *fix) {
    f->fix = fix;
    if (fix)
        fix->budget = &f->budget;
}

const fsck_result_t *fsck_result(const fsck_t *f) {
    return &f->res;
}
