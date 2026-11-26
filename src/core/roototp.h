// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The root secret in one-time-programmable (OTP) memory: where it is, how a page is told to
// hold one, and which source a board takes its root secret from (issue #64,
// docs/securing-a-board.md). This firmware only READS it. Writing it is the job of a separate
// image used once per board (the burn image); the format here is what that image must write.
//
// The page list. The firmware knows three OTP pages, tried in order: pages 4, 5 and 6 (page 3
// holds SOS's own flags, core/sealedonly.h). A page is 64 rows:
//
//   rows  0..15   the secret: 32 bytes, 16 bits a row with error correction, row k holding
//                 bytes 2k (low half) and 2k+1
//   rows 16..17   a check over the secret: the first 4 bytes of
//                 HMAC-SHA256(key = the secret, "SOS root secret check v1"), same layout
//   row  18       the marker 0xa55a, WRITTEN LAST
//   rows 32..47   chaff: the bitwise complement of the raw bits of rows 0..15, against reading
//                 the secret off the chip by imaging it (datasheet section 13.8.2). Never read
//                 as data.
//   the rest      blank
//
// What a page is:
//   blank        every row reads zero: never written
//   (a blank page that the firmware can't write, because its lock word says so, counts as
//   spent: it can never hold a secret, and treating it as blank would stop the search at a
//   page nothing can fill. The burn image never makes one; this is so that a lock alone can't
//   block the list.)
//   complete     the marker row reads 0xa55a. Its secret is used if the check matches.
//   spent        something is written and the marker row is still blank: a write that stopped
//                before the marker. Skipped for good.
//   damaged      the marker row holds something that isn't the marker
//   unreadable   a row can't be read (it comes back as all ones)
//
// Finding the secret: the first page of the list that isn't spent decides. Complete: that is
// the secret. Blank: there is none yet. Damaged or unreadable: there is none that can be used,
// and the search stops there. All three spent: there is none and there can't be one.
//
// A complete page whose check doesn't match (damage, or a read disturbed on purpose) is not a
// secret and is not skipped either: the board has no keys until that is looked into.
//
// Locks. A spent page holds 32 bytes and no marker. If the USB bootloader could still write
// it, someone with a cable could read those bytes, add the check and the marker, and make it
// the board's root secret: one they know. So every page of the list up to and including the
// secret's must be locked against the bootloader's writes (the page's permanent lock word,
// LOCK_BL not "read-write"; the burn image sets it). The firmware reads those lock words at
// boot (roototp_locks_ok). A sealed-only board whose pages aren't locked so has no source; an
// open board is warned.
//
// What that check is worth: it catches a board that was LEFT unlocked. It is no proof of the
// board's history. The lock words themselves can be written from the bootloader, so someone
// who completes a spent page can set its lock afterwards, and the check then passes. What
// prevents the attack is the burn locking each spent page in the same sitting in which it was
// spent, before the board is out of its owner's hands.
//
// Which source a board uses (roototp_choose):
//   - a secret in OTP: OTP (but not on a sealed-only board whose list pages the bootloader can
//     still write: see "Locks" above);
//   - a sealed-only board (core/sealedonly.h) with none: NO source. Never the flash record:
//     anyone with a cable can write one, with a configuration sealed under it;
//   - an open board with none (blank, or the list spent): the flash record (core/rootflash.h),
//     as before;
//   - an open board whose OTP pages can't be read, or hold a damaged secret or marker: no
//     source, said plainly, instead of quietly becoming a different board with a new flash
//     secret.
//
// No hardware in this file: the rows come through roototp_reader_t. Unit tested on the host.

#ifndef SOS_ROOTOTP_H
#define SOS_ROOTOTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ROOTOTP_SECRET_LEN  32
#define ROOTOTP_PAGES       3
#define ROOTOTP_FIRST_PAGE  4           // pages 4, 5, 6
#define ROOTOTP_PAGE_ROWS   64
#define ROOTOTP_SECRET_ROWS 16
#define ROOTOTP_CHECK_ROW   16
#define ROOTOTP_CHECK_ROWS  2
#define ROOTOTP_CHECK_LEN   4
#define ROOTOTP_MARKER_ROW  18
#define ROOTOTP_MARKER      0xa55au
#define ROOTOTP_CHAFF_ROW   32

// The OTP row number of row `row` of the list's page `index` (0..ROOTOTP_PAGES-1).
#define ROOTOTP_ROW(index, row) ((ROOTOTP_FIRST_PAGE + (index)) * ROOTOTP_PAGE_ROWS + (row))

typedef struct {
    void *ctx;
    // A row's 24 raw bits, or 0xffffffff if it can't be read. Never faults.
    uint32_t (*raw)(void *ctx, unsigned row);
    // A row's 16 data bits after error correction. Never faults; for the marker only.
    uint16_t (*ecc)(void *ctx, unsigned row);
    // `count` rows' data bits from `row`, read the careful way (on the board: the guarded read
    // alias, which stops the chip on a failed check instead of returning something). False if
    // it can't.
    bool (*careful)(void *ctx, unsigned row, unsigned count, uint16_t *out);

    // The raw lock word of OTP page `page` (its PAGEn_LOCK1 row: always readable), or
    // 0xffffffff if it can't be read.
    uint32_t (*lock1)(void *ctx, unsigned page);
} roototp_reader_t;

typedef enum {
    ROOTOTP_BLANK, ROOTOTP_SPENT, ROOTOTP_COMPLETE, ROOTOTP_DAMAGED, ROOTOTP_UNREADABLE,
} roototp_page_t;

roototp_page_t roototp_page(const roototp_reader_t *r, unsigned index);

typedef enum {
    ROOTOTP_FOUND,          // *index is the page holding the secret
    ROOTOTP_NONE_YET,       // the next page to use is blank: no secret has been written
    ROOTOTP_EXHAUSTED,      // every page of the list is spent
    ROOTOTP_CANT_READ,      // a page that would decide can't be read
    ROOTOTP_BAD_MARKER,     // a page's marker row is neither blank nor the marker
} roototp_find_t;

roototp_find_t roototp_find(const roototp_reader_t *r, unsigned *index);

// The check stored with a secret.
bool roototp_check(const uint8_t secret[ROOTOTP_SECRET_LEN], uint8_t check[ROOTOTP_CHECK_LEN]);

// Read the secret of a complete page and verify its check. False, with out zeroed, if the
// rows can't be read or the check doesn't match.
bool roototp_read(const roototp_reader_t *r, unsigned index, uint8_t out[ROOTOTP_SECRET_LEN]);

typedef enum {
    ROOTSRC_OTP,            // the secret in OTP
    ROOTSRC_FLASH,          // the flash record of an open board
    ROOTSRC_NONE,           // no root secret: no keys
} rootsrc_t;

// The page lock word's LOCK_BL field says the bootloader may write the page (or the word can't
// be read, which counts the same).
bool roototp_bootloader_can_write(uint32_t lock1_raw);

// The lock word's LOCK_S field says Secure code (the firmware) may write the page. A word that
// can't be read counts as "may not".
bool roototp_secure_can_write(uint32_t lock1_raw);

// Are all pages of the list up to and including `index` locked against the bootloader's
// writes? If not, *open_page (may be NULL) is the first that isn't, as an OTP page number.
bool roototp_locks_ok(const roototp_reader_t *r, unsigned index, unsigned *open_page);

// Which source this board uses. `otp_secret_ok`: for ROOTOTP_FOUND, whether roototp_read()
// succeeded; `locks_ok`: for ROOTOTP_FOUND, roototp_locks_ok() for that page.
rootsrc_t roototp_choose(roototp_find_t found, bool otp_secret_ok, bool locks_ok,
                         bool sealed_only);

#endif
