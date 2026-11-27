// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// fsck: a consistency checker for the card's FAT32 or exFAT volume, run on an unmounted card.
// Pure C: it knows nothing of SPI, FatFs or the shell. It reads sectors through a callback and
// reports problems as text lines, so the same code runs on the board and on image files in
// tests/test_fsck.c. docs/design/sd-check.md has the design; the rules it leaves open are
// settled in "Rules" below, and for exFAT's own structures in fsck_exfat.h, whose pieces this
// file strings together.
//
// All memory comes from the caller's buffer, and the work is done a bounded number of sector
// reads at a time (fsck_step()), so the board's main loop keeps running during a check.
//
// Rules decided here (the design note leaves them open):
// - Volume: found as FatFs's find_volume() finds it (no partition table, GPT, then MBR). The
//   boot sector is refused for what FatFs's mount_volume() refuses, plus two things it can't
//   check without them: a volume that runs past the end of the device, and a root directory
//   cluster out of range.
// - Backup boot sector: the one BPB_BkBootSec names, compared with the boot sector when it is
//   inside the reserved area and isn't the FSInfo sector; 0 means there is none.
// - FSInfo: read only when BPB_FSInfo32 is 1, as FatFs reads it. Wrong signatures are a
//   problem; a free count that isn't 0xFFFFFFFF and differs from the clusters counted free is
//   a note.
// - FAT entries: on FAT32 the upper 4 bits are ignored, as FatFs ignores them. A chain ends at
//   0x0FFFFFF8 or above (exFAT: 0xFFFFFFF8 or above, all 32 bits read). 0 (free) and the bad
//   mark (0x0FFFFFF7, exFAT 0xFFFFFFF7) break it, as does a value below 2 or above the cluster
//   count + 1. A cluster whose entry is free or bad is not counted in the chain, so it is
//   reported once, as the break, and never as "reached but free".
// - Sizes are compared only for chains that end properly; a broken chain is reported as
//   broken, not also as too short.
// - Directories: a directory whose chain loops is not entered. One whose chain breaks is read
//   up to the break. A directory entry with start cluster 0 is read by FatFs as the root, so it
//   is reported as a loop back to "/". Only the first 65536 entries are read (FatFs's MAX_DIR,
//   2 MB). The stack holds the root and 63 levels below it: a directory 64 levels down is
//   reported and its chain counted, but its entries are not read, so what it holds shows up as
//   lost clusters.
// - Entries FatFs skips are skipped: deleted entries, volume labels, and entries whose name
//   starts with '.' other than the "." and ".." entries (those are reported as bad names).
// - "." and "..": in a subdirectory entries 0 and 1 must be "." (the directory's own cluster)
//   and ".." (the parent's, 0 for the root; the root's own cluster number is accepted too, as
//   FatFs accepts it). Anywhere else, and anywhere in the root, they are reported.
// - Case on exFAT: name hashes are computed, and names compared, with io->upcase, which the
//   board sets to FatFs's ff_wtoupper(). NULL means fsck_upcase() below, a subset: then names
//   with letters outside it get a hash FatFs computes differently (reported as wrong), and the
//   volume's up-case table is noted as differing from it. FAT32 always uses fsck_upcase().
// - Names: a long name with a character FatFs refuses (controls, 0x7F, "*/:<>?\|) is reported;
//   so is a short name with a control (other than 0x05 first), 0x7F, a space first, or one of
//   "*+,./:;<=>?[\]|, which FatFs never writes into a short name.
// - Repeated names: two entries in a directory with the same short name, or with the same long
//   name (a short name stands in for a missing long one) compared case-insensitively. Case is
//   folded for ASCII, Latin-1, Latin Extended-A, Greek and Cyrillic exactly as FatFs folds it
//   (fsck_upcase()); other letters compare as they are, so a repeat that differs only in case
//   in another script isn't seen. That keeps FatFs's 1 KB case table out of this file. exFAT:
//   two File entry sets with the same name, compared unit by unit after io->upcase, as FatFs's
//   dir_find() compares them. In pass 1 each directory is read once more before its entries are
//   followed: a Bloom filter of the names seen so far picks candidates, and a rescan of the
//   directory up to the entry confirms each one. Rescans stop, with a note, once they have read
//   four times the directory's size.
// - Cross-links: reported once for each entry that reaches a cluster an earlier entry (in walk
//   order) already uses, naming that entry only. With one successor per FAT entry, chains that
//   share any cluster share their last one, so the line is printed in the pass whose window holds
//   that last cluster: the same lines whatever the window size. Cross-linked clusters are
//   counted one by one. A chain that loops is reported as looping and not as cross-linked.
// - exFAT cross-links: a NoFatChain run can share any part of another entry's clusters, so
//   the FAT32 reasoning doesn't hold. An entry is named when its last cluster (of its run, or
//   the last its chain reaches) was already used by an earlier entry, in the pass whose window
//   holds that cluster. Every re-used cluster is counted in cross_linked; if some are and no
//   entry was named, one line gives their number. The lines stay the same whatever the window.
// - Lost clusters (FAT32: used in the FAT, reached by nothing, not marked bad; exFAT: set in the
//   allocation bitmap and reached by nothing) are reported as runs of consecutive cluster
//   numbers; a run that crosses a window boundary is still one line. On exFAT, clusters reached
//   and clear in the bitmap are reported the same way, as "reached but free"; the FAT isn't
//   used for accounting, and bad clusters aren't counted.
// - exFAT walk: the root is read along its FAT chain; a subdirectory for DataLength / 32
//   entries, contiguously when NoFatChain is set, else along its chain, and at most FatFs's
//   MAX_DIR_EX entries. A set with problems is still followed when its stream entry was read
//   and its first cluster is in range (FatFs refuses it, but the bitmap holds its clusters).
//   The chains of directories, the bitmap and the up-case table are checked against their
//   DataLength like files'. The allocation bitmap is read as FatFs reads it, contiguously from
//   its first cluster, so its FAT chain must run on cluster by cluster to 0xFFFFFFFF (FatFs
//   refuses the volume otherwise: a problem); without a usable bitmap entry the clusters are
//   not compared at all. The up-case table is read once, in pass 1.
// - Notes are report lines too, starting "note: ". They share the FSCK_REPORT_MAX limit with the
//   problems but are counted apart, and they don't make "no problems" false. They are: a stale
//   FSInfo free count, a chain longer than its file (also counted in long_chains, the clusters
//   repair trims), FAT entry 0 not matching the media byte, the dirty and disk-error bits of
//   FAT entry 1 or of exFAT's VolumeFlags, an up-case table that maps characters unlike
//   io->upcase, and rescans for repeated names cut short.
// - Work: in each pass, after following 4 * clusters + 64 chain steps (a NoFatChain run counts
//   its clusters) the walk stops with a
//   problem saying the result is incomplete. A volume whose chains don't overlap can't get
//   there; this bounds the work on a hostile or badly damaged card.
// - Summary: "no problems" or "N problems; <size> lost, <n> cross-linked", then "; <n> bad
//   clusters" when there are any and "; volume flagged dirty" when FAT entry 1 or VolumeFlags
//   says so.

#ifndef SOS_FSCK_H
#define SOS_FSCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FSCK_REPORT_MAX   50    // report lines (problems and notes); later ones are only counted
#define FSCK_DEPTH_MAX    64    // directory levels read, the root included
#define FSCK_LINE_MAX     320   // longest report line, with its terminator
#define FSCK_SUMMARY_MAX  160
#define FSCK_WINDOW_MIN   16    // bytes of cluster bitmap at least: 128 clusters per pass

typedef struct {
    // Read `count` 512-byte sectors starting at `lba` (in the decrypted volume) into `buf`.
    bool (*read)(void *ctx, uint64_t lba, uint32_t count, uint8_t *buf);
    // One problem or note, already formatted, without a newline. Called at most
    // FSCK_REPORT_MAX times; later lines are only counted.
    void (*report)(void *ctx, const char *line);
    // Write sectors: NULL for a check. Only repair (design note §10) will pass one.
    bool (*write)(void *ctx, uint64_t lba, uint32_t count, const uint8_t *buf);
    void *ctx;
    uint64_t sectors;           // size of the device as the layer shows it
    // FatFs's up-case conversion, ff_wtoupper(), for exFAT names (see "Case on exFAT"
    // above); NULL: the subset in fsck_upcase().
    uint32_t (*upcase)(uint32_t c);
} fsck_io_t;

typedef enum { FSCK_KIND_NONE, FSCK_KIND_FAT32, FSCK_KIND_EXFAT } fsck_kind_t;

typedef struct {
    const char *why;            // FSCK_FAILED: why the check couldn't go on; else NULL
    fsck_kind_t kind;           // known once the volume is found
    uint64_t volume_lba;        // where the volume starts on the device
    uint64_t volume_sectors;
    uint32_t clusters;          // cluster count
    uint32_t cluster_bytes;
    uint32_t passes;            // passes the check takes (known once the volume is found)
    uint32_t pass;              // the pass running, from 1
    uint32_t problems;          // problems found, reported or not
    uint32_t notes;
    uint32_t long_chains;       // notes for chains longer than their file (repair trims them)
    uint32_t reported;          // lines given to report()
    uint32_t files, dirs;       // entries walked (the root counts as a directory)
    uint32_t free_clusters;     // FAT entries 0; exFAT: clear in the allocation bitmap
    uint32_t bad_clusters;      // FAT32's entries 0x0FFFFFF7
    uint32_t lost_clusters;     // used in the FAT (exFAT: the bitmap), reached by nothing
    uint32_t reached_free;      // exFAT: reached, but clear in the allocation bitmap
    uint32_t cross_linked;      // clusters reached by a second entry
    bool dirty;                 // FAT entry 1's clean-shutdown bit clear, or exFAT's dirty flag
    char summary[FSCK_SUMMARY_MAX];  // once FSCK_DONE: "no problems" or "N problems; ..."
} fsck_result_t;

typedef struct fsck fsck_t;     // the checker's state, placed in the caller's memory

enum { FSCK_MORE, FSCK_DONE, FSCK_FAILED };

// The fixed part of the checker's memory: about 7 KB (three sector buffers, two long names,
// the directory stack of 64 x 28 bytes, a 512-byte Bloom filter, exFAT's entry set reader,
// the path and the report line). fsck_new() uses the rest of `mem` as the cluster window: a bitmap of W bytes covers
// 8W clusters per pass.
size_t fsck_state_size(void);

// Set up a check of the device `io` describes (`io` is copied; a check never calls
// `io->write`) in `mem`, which must hold fsck_state_size() + FSCK_WINDOW_MIN bytes. NULL if it
// doesn't, or if `io` lacks a read callback or a size.
fsck_t *fsck_new(const fsck_io_t *io, void *mem, size_t mem_len);

// Do a bounded amount of work: at most `budget` sector reads (at least 1). FSCK_MORE: call
// again; FSCK_DONE: finished, see fsck_result(); FSCK_FAILED: couldn't go on (read error, no
// volume, a boot sector FatFs would refuse), see fsck_result()->why. After DONE or FAILED
// it returns the same again without doing anything.
int fsck_step(fsck_t *f, uint32_t budget);

const fsck_result_t *fsck_result(const fsck_t *f);

// FatFs's up-case mapping (ff_wtoupper()) for U+0000-U+017F (ASCII, Latin-1, Latin
// Extended-A) and U+0370-U+052F (Greek, Cyrillic); every other code unit maps to itself. Used
// to compare names; public so the tests can hold it against FatFs's.
uint16_t fsck_upcase(uint16_t c);

#endif
