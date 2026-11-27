// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// fsck_exfat: the exFAT parts of the card checker (docs/design/sd-check.md §4, §5 "exFAT", §6),
// as pieces a directory walker calls entry by entry and sector by sector. Pure C, like fsck.c:
// no FatFs, no I/O, no malloc, no recursion. Every function works on the caller's buffers and
// state structs, and reports problems as bits that the *_text() functions turn into report
// lines (without the path, which only the walker knows).
//
// The pieces:
// - Boot region: fsck_exfat_vbr() parses and refuses a boot sector as FatFs's mount_volume()
//   does; fsck_exfat_boot_*() take the 24 sectors of the main and backup boot regions in order
//   and check the boot checksum (sector 11 against sectors 0-10) and the backup's sameness.
// - Entry sets: fsck_exfat_entry() takes one 32-byte directory entry at a time and hands back
//   each finished set (fsck_exfat_set_t): a file or directory, or one of the root's critical
//   entries.
// - Up-case table: fsck_exfat_upcase_*() take the table's bytes and check its checksum.
// - Cluster accounting: fsck_window_mark_run() marks a run of clusters into a window bitmap and
//   counts the cross-links; fsck_bitcmp_*() compare a window with the allocation bitmap and give
//   the lost and reached-but-free runs. Neither is exFAT-specific: FAT32 can use fsck_run_add().
//
// Rules decided here (the design note leaves them open; where it and FatFs differ, FatFs wins):
// - Name hash: computed with FatFs's own up-case rule, ff_wtoupper(), passed in as a function
//   (fsck_exfat_upcase_fn) so this file needn't link FatFs. FatFs finds a name by comparing
//   that hash (dir_find()), and never reads the volume's up-case table; so a stored hash that
//   differs from it is a file FatFs can't open, whatever the table says. The table itself is
//   streamed and only its checksum is held (no 128 KB copy): a wrong checksum is a problem,
//   and a table that maps any character differently from ff_wtoupper() is a note (FatFs's
//   f_mkfs builds its table from ff_wtoupper(), so on a card SOS formatted they agree).
// - Entry sets are checked as FatFs's load_xdir() reads them: 2 to 18 secondaries, a Stream
//   Extension second, then only File Name entries. The exFAT spec also allows benign
//   secondaries (vendor extensions) after the names; FatFs can't read such a set, so they are
//   reported. Name entries must number exactly ceil(name length / 15), and at least one
//   (FatFs accepts more).
// - A set that has problems is still handed back when its stream entry was read (`usable`), so
//   the walker can account for its clusters: FatFs refuses the set, but the bitmap still holds
//   them. The checksum and name checks are skipped for a set cut short (which is reported
//   instead), the hash check when the name entries don't match the name length.
// - Names: a character FatFs refuses in a name (controls, 0x7F, "*/:<>?\|), a broken
//   surrogate pair (FatFs shows the name as "?"), an empty name, and "." or "..".
// - Sizes: DataLength 0 must have first cluster 0; otherwise the first cluster is 2 ... count+1
//   and ceil(DataLength / cluster size) clusters must fit the heap. For a NoFatChain set the
//   run itself must end inside the heap. ValidDataLength must not exceed DataLength.
// - Unknown entry types: an unknown critical primary (0x80-0x9F) is reported and so is a
//   critical secondary (0xC0-0xDF) outside a set, once per run of them and not right after a
//   set whose own count was wrong (they are its leftovers). Benign primaries (0xA0-0xBF) are
//   skipped with their secondaries, and benign secondaries outside a set are skipped, as FatFs
//   skips them. Deleted entries (0x01-0x7F) are skipped; inside a set they cut it short.
// - The root's critical entries: exactly one Allocation Bitmap (0x81), in the root's first
//   cluster (FatFs only looks there and won't mount without it), at least ceil(count / 8)
//   bytes long; exactly one Up-case Table (0x82), 2 to 131072 bytes, even; at most one volume
//   label (0x83), of at most 11 characters. 0x81-0x83 outside the root are reported.
// - Boot region: the checksum is over sectors 0-10 without VolumeFlags and PercentInUse (bytes
//   106, 107, 112 of sector 0), as f_mkfs computes it; every dword of sector 11 must hold it.
//   The backup is compared sector by sector with the main region (those three bytes left out:
//   they may change in the main region only), through a 32-bit digest of each main sector, so
//   the 24 sectors are read once each, in order. Both are reported problems, not refusals.
// - Refusals (fsck_exfat_vbr()): what FatFs's mount_volume() refuses (non-zero bytes 11-63,
//   version not 1.0, sector size not 512, number of FATs not 1, cluster size over 2^15
//   sectors, more than 0x7FFFFFFD clusters, a heap that doesn't fit the volume), plus what the
//   design note adds: no clusters, a volume past the device's end, a FAT before sector 24, too
//   small for the clusters or overlapping the heap, and a root cluster out of range.

#ifndef SOS_FSCK_EXFAT_H
#define SOS_FSCK_EXFAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FSCK_EXFAT_NAME_MAX     255     // UTF-16 code units in a name
#define FSCK_EXFAT_TEXT_MAX     160     // longest *_text() line, with its terminator

// FatFs's up-case conversion, ff_wtoupper() (ffunicode.c): the board passes that function.
typedef uint32_t (*fsck_exfat_upcase_fn)(uint32_t c);

// --- boot region ----------------------------------------------------------------------------

typedef struct {
    uint64_t volume_lba;        // where the boot sector was found on the device
    uint64_t volume_sectors;    // VolumeLength
    uint32_t fat_offset;        // FatOffset, in sectors from the volume's start
    uint32_t fat_sectors;       // FatLength
    uint32_t heap_offset;       // ClusterHeapOffset, in sectors from the volume's start
    uint32_t clusters;          // ClusterCount: clusters 2 ... clusters + 1
    uint32_t root_cluster;      // FirstClusterOfRootDirectory
    uint32_t cluster_sectors;
    uint32_t cluster_bytes;
    uint16_t volume_flags;      // VolumeFlags (bit 1: dirty, bit 2: media failure)
} fsck_exfat_geom_t;

#define FSCK_EXFAT_FLAG_DIRTY   0x0002u

// FatFs's check_fs() for exFAT: the jump and "EXFAT   " name in bytes 0-10, and 55 AA.
bool fsck_exfat_is_vbr(const uint8_t sec[512]);

// Parse the boot sector `sec`, found at `volume_lba` on a device of `device_sectors`, into `g`.
// NULL if the volume can be checked; otherwise why not (the boot sector is refused).
const char *fsck_exfat_vbr(const uint8_t sec[512], uint64_t volume_lba, uint64_t device_sectors,
                           fsck_exfat_geom_t *g);

// The main and backup boot regions, fed as the volume's sectors 0 to 23, in order.
typedef struct {
    uint32_t sum;               // boot checksum of the region being fed
    uint32_t digest[12];        // of each main sector, to compare the backup with
    uint32_t next;              // the sector expected next (0-23; 24 when done)
    uint32_t problems;          // FSCK_EXFAT_B_* found so far
    uint32_t first_diff;        // FSCK_EXFAT_B_BACKUP: the first backup sector that differs
} fsck_exfat_boot_t;

#define FSCK_EXFAT_B_CHECKSUM   0x1u    // sector 11 doesn't hold the checksum of sectors 0-10
#define FSCK_EXFAT_B_BACKUP     0x2u    // the backup boot region differs from the main one

void fsck_exfat_boot_init(fsck_exfat_boot_t *b);
// The volume's next sector (0, 1, ... 23); further sectors are ignored.
void fsck_exfat_boot_sector(fsck_exfat_boot_t *b, const uint8_t sec[512]);
// FSCK_EXFAT_B_* found so far: all of them once the 24 sectors are in.
uint32_t fsck_exfat_boot_done(const fsck_exfat_boot_t *b);
// The report line for one FSCK_EXFAT_B_* bit.
void fsck_exfat_boot_text(const fsck_exfat_boot_t *b, uint32_t bit, char *out, size_t n);

// --- entry sets -----------------------------------------------------------------------------

typedef enum {
    FSCK_EXFAT_FILE,            // a File entry set: a file or a directory
    FSCK_EXFAT_BITMAP,          // the root's Allocation Bitmap entry
    FSCK_EXFAT_UPCASE,          // the root's Up-case Table entry
    FSCK_EXFAT_LABEL,           // the root's volume label entry
} fsck_exfat_kind_t;

// Problems with one set (fsck_exfat_set_t.problems).
#define FSCK_EXFAT_S_SHORT          0x00001u  // cut short before its last secondary
#define FSCK_EXFAT_S_COUNT          0x00002u  // secondary count not 2 to 18
#define FSCK_EXFAT_S_NO_STREAM      0x00004u  // second entry not a Stream Extension
#define FSCK_EXFAT_S_NOT_NAME       0x00008u  // a later secondary not a File Name entry
#define FSCK_EXFAT_S_CHECKSUM       0x00010u  // SetChecksum wrong
#define FSCK_EXFAT_S_NAME_ENTRIES   0x00020u  // name entries don't match the name length
#define FSCK_EXFAT_S_HASH           0x00040u  // NameHash wrong
#define FSCK_EXFAT_S_NAME_CHAR      0x00080u  // a character FatFs refuses, or a broken pair
#define FSCK_EXFAT_S_NAME_EMPTY     0x00100u  // name length 0
#define FSCK_EXFAT_S_NAME_DOT       0x00200u  // "." or ".."
#define FSCK_EXFAT_S_VALID_LENGTH   0x00400u  // ValidDataLength > DataLength
#define FSCK_EXFAT_S_FIRST_CLUSTER  0x00800u  // first cluster out of range (or set for size 0)
#define FSCK_EXFAT_S_TOO_BIG        0x01000u  // more clusters than the heap has
#define FSCK_EXFAT_S_PAST_HEAP      0x02000u  // NoFatChain run past the heap's last cluster
#define FSCK_EXFAT_S_EXTRA          0x04000u  // a second bitmap, up-case table or label
#define FSCK_EXFAT_S_NOT_FIRST      0x08000u  // bitmap entry past the root's first cluster
#define FSCK_EXFAT_S_BITMAP_SIZE    0x10000u  // bitmap smaller than ceil(clusters / 8)
#define FSCK_EXFAT_S_UPCASE_SIZE    0x20000u  // up-case table not 2 to 131072 bytes, even
#define FSCK_EXFAT_S_LABEL_LENGTH   0x40000u  // label over 11 characters

typedef struct {
    fsck_exfat_kind_t kind;
    uint32_t problems;          // FSCK_EXFAT_S_*
    uint32_t index;             // the primary entry's index in its directory, from 0
    bool usable;                // the cluster fields below were read (a stream entry, or a
                                // bitmap or up-case entry): the walker can follow them
    bool dir;                   // FILE: the directory attribute
    bool no_fat_chain;          // FILE: contiguous, the FAT not used (GeneralSecondaryFlags bit 1)
    uint16_t attributes;        // FILE: FileAttributes
    uint32_t first_cluster;
    uint64_t length;            // DataLength
    uint64_t valid_length;      // FILE: ValidDataLength
    uint32_t clusters;          // ceil(length / cluster size), at most the heap's count
    uint32_t run_clusters;      // no_fat_chain: of those, the ones inside the heap
    uint32_t table_checksum;    // UPCASE: TableChecksum, for fsck_exfat_upcase_done()
    // FILE: the name, as stored (name_len code units, of which name_have were read)
    uint8_t name_len;
    uint8_t name_have;
    uint16_t name[FSCK_EXFAT_NAME_MAX];
    // For the report lines: what the set says and what was found
    uint8_t secondaries;        // SecondaryCount
    uint8_t seen;               // secondaries read (S_SHORT: fewer than `secondaries`)
    uint8_t name_entries;       // File Name entries in the set
    uint8_t bad_type;           // S_NO_STREAM, S_NOT_NAME: the type found
    uint8_t bad_pos;            // S_NOT_NAME: its position in the set (1 = first secondary)
    uint16_t bad_char;          // S_NAME_CHAR: the character (0xD800-0xDFFF: a broken pair)
    uint16_t sum_stored, sum_found;
    uint16_t hash_stored, hash_found;
} fsck_exfat_set_t;

// Problems with a directory rather than a set (fsck_exfat_dir_t.problems).
#define FSCK_EXFAT_D_UNKNOWN        0x01u     // an unknown critical primary entry
#define FSCK_EXFAT_D_ROOT_ONLY      0x02u     // a bitmap, up-case or label entry not in the root
#define FSCK_EXFAT_D_STRAY          0x04u     // a critical secondary outside any set
#define FSCK_EXFAT_D_NO_BITMAP      0x08u     // the root has no Allocation Bitmap entry
#define FSCK_EXFAT_D_NO_UPCASE      0x10u     // the root has no Up-case Table entry

// The state of one directory being read. Holds the set being read (about 560 bytes).
typedef struct {
    const fsck_exfat_geom_t *geom;
    fsck_exfat_upcase_fn upcase;
    fsck_exfat_set_t set;       // the set being read; complete when FSCK_EXFAT_SET is returned
    uint32_t problems;          // FSCK_EXFAT_D_* from the last call
    uint32_t bad_index;         // D_UNKNOWN, D_ROOT_ONLY, D_STRAY: the entry's index
    uint8_t bad_type;           //   and its type
    bool root;
    bool in_set;                // reading a set's secondaries
    bool ended;                 // the end-of-directory entry was read
    bool in_stray;              // the last entry was a stray critical secondary
    bool leftovers;             // the last set's count was wrong: strays after it are its own
    uint8_t skip;               // secondaries of a benign primary still to skip
    uint16_t sum;               // SetChecksum so far
    uint32_t index;             // the next entry's index
    uint32_t bitmaps, upcases, labels;
} fsck_exfat_dir_t;

// fsck_exfat_entry() and fsck_exfat_dir_end() results: a mask.
#define FSCK_EXFAT_SET      0x1u    // d->set is a finished set: account for it, report its problems
#define FSCK_EXFAT_DIR      0x2u    // d->problems has directory problems to report
#define FSCK_EXFAT_END      0x4u    // the end-of-directory entry: read no further
#define FSCK_EXFAT_AGAIN    0x8u    // the entry cut a set short: pass the same entry again

// Start reading a directory (`root`: the root directory). `geom` and `upcase` must stay valid.
void fsck_exfat_dir_begin(fsck_exfat_dir_t *d, const fsck_exfat_geom_t *geom,
                          fsck_exfat_upcase_fn upcase, bool root);
// The directory's next 32-byte entry. Returns FSCK_EXFAT_* bits; with FSCK_EXFAT_AGAIN, handle
// the set, then pass the same entry again.
uint32_t fsck_exfat_entry(fsck_exfat_dir_t *d, const uint8_t e[32]);
// The directory's chain ended (or its end entry was read): a set still open is handed back cut
// short, and the root's missing critical entries are reported.
uint32_t fsck_exfat_dir_end(fsck_exfat_dir_t *d);
// The report line for one FSCK_EXFAT_S_* bit of `s`, or one FSCK_EXFAT_D_* bit of `d`. The
// walker puts the path first: the directory's, then the set's name when name_have isn't 0 (a
// name cut short is shown as far as it was read). A line about a set without a name, or about
// one of the root's critical entries, starts with the entry's index ("file entry 12: ...").
// The kind of a root critical entry, as a report line names it ("allocation bitmap" ...).
const char *fsck_exfat_kind_name(fsck_exfat_kind_t k);
void fsck_exfat_set_text(const fsck_exfat_set_t *s, uint32_t bit, char *out, size_t n);
void fsck_exfat_dir_text(const fsck_exfat_dir_t *d, uint32_t bit, char *out, size_t n);

// FatFs's xname_sum() over `len` code units of `name`, up-cased with `upcase`.
uint16_t fsck_exfat_name_hash(const uint16_t *name, size_t len, fsck_exfat_upcase_fn upcase);

// --- up-case table --------------------------------------------------------------------------

// The table, fed as it is read: its checksum, and how it compares with FatFs's ff_wtoupper().
// Compressed (0xFFFF then a count of characters mapping to themselves) or not.
typedef struct {
    fsck_exfat_upcase_fn upcase;
    uint32_t length;            // DataLength: bytes to take
    uint32_t fed;               // bytes taken so far
    uint32_t sum;               // TableChecksum so far
    uint32_t code;              // the character the next mapping is for (0x10000: all done)
    uint32_t differ;            // characters the table maps unlike ff_wtoupper()
    uint32_t first_differ;      // the first of them
    uint32_t stored;            // the entry's TableChecksum, once done
    uint16_t low;               // the low byte of a code unit split between two feeds
    bool odd;                   //   (set when one is pending)
    bool count_next;            // the last unit was 0xFFFF: the next is a count
} fsck_exfat_upcase_t;

#define FSCK_EXFAT_U_CHECKSUM   0x1u    // problem: the checksum isn't the entry's TableChecksum
#define FSCK_EXFAT_U_DIFFERS    0x2u    // note: maps characters unlike FatFs's ff_wtoupper()

void fsck_exfat_upcase_init(fsck_exfat_upcase_t *u, uint32_t length, fsck_exfat_upcase_fn upcase);
// The table's next `n` bytes; bytes past `length` are ignored.
void fsck_exfat_upcase_feed(fsck_exfat_upcase_t *u, const uint8_t *buf, size_t n);
// After the whole table: FSCK_EXFAT_U_* against the entry's `table_checksum`. Characters past
// the table's end map to themselves, so they are compared too (at most 65536 upcase() calls in
// all, feeds included).
uint32_t fsck_exfat_upcase_done(fsck_exfat_upcase_t *u, uint32_t table_checksum);
void fsck_exfat_upcase_text(const fsck_exfat_upcase_t *u, uint32_t bit, char *out, size_t n);

// --- cluster accounting (exFAT and FAT32) ---------------------------------------------------

// A run of consecutive clusters.
typedef struct {
    uint32_t first, count;
} fsck_run_t;

// Add cluster `c` to the open run `r`. If `c` doesn't follow it, the run ends: it is copied to
// `*done` (true returned) and a new one starts at `c`.
bool fsck_run_add(fsck_run_t *r, uint32_t c, fsck_run_t *done);
// End the open run: copied to `*done`, true if there was one.
bool fsck_run_end(fsck_run_t *r, fsck_run_t *done);

// Mark `count` clusters from `first` in the window `win` (bit i: cluster `win_first` + i, for
// `win_bits` clusters); those outside the window are left alone. Returns how many were set
// already (cross-linked), the first of them in `*cross` (unchanged when none).
uint32_t fsck_window_mark_run(uint8_t *win, uint32_t win_first, uint32_t win_bits,
                              uint32_t first, uint32_t count, uint32_t *cross);

// A window of reached clusters against the allocation bitmap, a slice at a time.
typedef struct {
    fsck_run_t lost, freed;     // the open runs, carried into the next slice
    const uint8_t *reached, *bitmap;
    uint32_t first, bits, pos;  // the slice: its first cluster, its length, the next bit
    uint32_t lost_total;        // clusters set in the bitmap and not reached
    uint32_t free_total;        // clusters reached and clear in the bitmap
    uint32_t used_total;        // clusters set in the bitmap
} fsck_bitcmp_t;

#define FSCK_RUN_LOST   1       // set in the bitmap, not reached
#define FSCK_RUN_FREE   2       // reached, clear in the bitmap

void fsck_bitcmp_init(fsck_bitcmp_t *c);
// Compare `bits` clusters from cluster `first`: bit i of `reached` and of `bitmap` stand for
// cluster `first` + i. Both buffers must stay until fsck_bitcmp_next() returns 0.
void fsck_bitcmp_slice(fsck_bitcmp_t *c, const uint8_t *reached, const uint8_t *bitmap,
                       uint32_t first, uint32_t bits);
// The slice's next finished run in `*run`: FSCK_RUN_LOST or FSCK_RUN_FREE; 0 when the slice is
// done (a run still open goes on into the next slice, if that starts where this one ended).
int fsck_bitcmp_next(fsck_bitcmp_t *c, fsck_run_t *run);
// After the last slice: the runs still open, one per call; 0 when none is left.
int fsck_bitcmp_finish(fsck_bitcmp_t *c, fsck_run_t *run);

#endif
