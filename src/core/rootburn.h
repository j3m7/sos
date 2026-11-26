// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Writing a board's root secret into one-time-programmable (OTP) memory: the steps, in the
// order that keeps every interruption safe. EVERY WRITE HERE IS PERMANENT.
//
// This is not part of everyday firmware. It is compiled only into the burn image
// (-DSOS_BURN_IMAGE=ON), which is flashed for this one job and replaced afterwards
// (docs/securing-a-board.md, step 3). It writes the format core/roototp.h reads.
//
// What rootburn_run() does, on the first blank page of the root secret's list:
//
//   0. Locks every list page before it. Those are spent pages (an earlier run that stopped):
//      each holds 32 bytes and no marker, and left writable from the USB bootloader could be
//      completed by someone with a cable into a root secret they know.
//   1. Takes 32 bytes from the random number generator. They never leave the board and are
//      never shown.
//   2. Writes them, error-corrected, a row at a time, reading each row back.
//   3. Writes the chaff: for each of those rows, the complement of its raw bits.
//      (With `rehearse`, it stops here on purpose: the page is spent, as a real interruption
//      would leave it, and is locked. docs/securing-a-board.md, the dev-board variant.)
//   4. Writes the check over the secret.
//   5. Reads the secret and the check back the careful way and verifies them.
//   6. Writes the marker. Only now does the page hold a root secret.
//   7. Locks the page: no access from the bootloader or Non-secure code, read-only for the
//      firmware.
//
// If anything fails after something was written, the page is spent. The run stops, locks the
// page, and reports where it stopped; running again uses the next page.
//
// A page that still reads blank is never locked. If the very first write fails and nothing
// landed, the page is left as it was, blank and unlocked: locking it would make it a page that
// can be neither written nor passed over, and the search for the secret stops at the first
// blank page, so the whole list would be blocked for good. (core/roototp.h skips a blank page
// that the firmware can't write, should one ever exist.)
//
// On a board that already has its secret, a run writes no secret; it locks the secret's page
// and the spent pages before it if an earlier run couldn't.
//
// A run that ends in an error other than "nobody held the button" is a reason to stop and
// find out why before running anything again (rootburn_must_stop): a fault in how OTP is
// written would otherwise spend the remaining pages the same way.
//
// No hardware in this file: OTP comes through rootburn_ops_t. Unit tested on the host against
// a model of OTP in which bits can be set and never cleared.

#ifndef SOS_ROOTBURN_H
#define SOS_ROOTBURN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/roototp.h"

// The lock byte written (three times over) into a page's lock word, PAGEn_LOCK1:
// Secure read-only (1), Non-secure inaccessible (3 << 2), bootloader inaccessible (3 << 4).
#define ROOTBURN_LOCK_BYTE 0x3du
#define ROOTBURN_LOCK_WORD (ROOTBURN_LOCK_BYTE | ROOTBURN_LOCK_BYTE << 8 | ROOTBURN_LOCK_BYTE << 16)

typedef struct {
    void *ctx;
    // Reading, as core/roototp.h.
    roototp_reader_t reader;
    // Program a row with error correction: the 16 data bits. True if the call succeeded.
    bool (*write_ecc)(void *ctx, unsigned row, uint16_t value);
    // Program a row's 24 raw bits to exactly `bits`. As the boot ROM does it: the call fails
    // if the row already has a bit set that `bits` leaves unset. Nothing is ever cleared.
    bool (*write_raw)(void *ctx, unsigned row, uint32_t bits);
    // Program OTP page `page`'s lock word (PAGEn_LOCK1) raw, as write_raw.
    bool (*write_lock1)(void *ctx, unsigned page, uint32_t bits);
    // Called before every write: keep the board from resetting meanwhile (feed the watchdog).
    // May be NULL.
    void (*tick)(void *ctx);
    // Fill out with len random bytes.
    bool (*random)(void *ctx, uint8_t *out, size_t len);
    // Is the person still there (the physical action the command asks for)?
    bool (*present)(void *ctx);
} rootburn_ops_t;

typedef enum {
    ROOTBURN_OK,                // the page holds the root secret and is locked
    ROOTBURN_REHEARSED,         // stopped on purpose after the chaff; the page is spent and locked
    ROOTBURN_RELOCKED,          // the secret was already there; a page left unlocked is locked now
    ROOTBURN_NOT_PRESENT,       // nobody held the button: nothing was written
    ROOTBURN_HAS_SECRET,        // a root secret is already there and locked: nothing was written
    ROOTBURN_NO_PAGE,           // every page is spent, or one can't be read or is damaged:
                                // nothing was written
    ROOTBURN_EARLIER_LOCK,      // locking an earlier, spent page failed: nothing else was written
    ROOTBURN_RANDOM,            // no usable random bytes: nothing was written
    ROOTBURN_WRITE,             // a write failed or didn't read back: the page is spent, unless
                                // nothing landed (rootburn_report_t.nothing_landed)
    ROOTBURN_VERIFY,            // the secret didn't verify before the marker: the page is spent
    ROOTBURN_LOCK,              // locking a page that holds something failed (`page` says which)
} rootburn_result_t;

typedef struct {
    rootburn_result_t result;
    unsigned page;              // the OTP page it worked on (meaningful unless nothing was written)
    unsigned row;               // for ROOTBURN_WRITE: the OTP row
    bool wrote;                 // something was written to OTP by this run
    bool page_locked;           // the page it worked on ended up locked
    bool nothing_landed;        // for ROOTBURN_WRITE: the first write failed and the page still
                                // reads blank. It was left unlocked and is the next run's page
} rootburn_report_t;

// Do it. `rehearse`: stop on purpose after step 3.
void rootburn_run(const rootburn_ops_t *ops, bool rehearse, rootburn_report_t *report);

// After this result, should nothing more be run until someone has looked into it?
bool rootburn_must_stop(rootburn_result_t result);

// A sentence for the result (no trailing newline).
const char *rootburn_result_text(rootburn_result_t result);

#endif
