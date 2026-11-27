// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// A count of wrong console passwords that survives a reboot (issue #64).
//
// The console delays the next password check after each wrong one (shell/login.h). Kept only in
// RAM, that count starts again at every boot, so pulling the power between guesses would skip
// the delays. Here it is kept in flash: the count of password checks STARTED since the last
// right password. It is recorded before a check begins, and cleared after a right one. So a
// check that was cut short by a reset counts as a wrong one, which is what makes cutting the
// power useless.
//
// What it is and isn't. It slows down guessing at the console for someone who doesn't rewrite
// the flash. While the USB bootloader is enabled, someone with a cable can erase this sector
// and start from zero; the slow password derivation is then the only limit, as it was before.
//
// How it is stored: one flash sector, written so that counting needs no erase. Flash bits can
// be cleared one at a time without erasing; an erase sets a whole sector's bits again and
// wears it.
//
//   bytes 0..7      a marker, so that whatever the sector held before isn't read as a count
//   attempts track  one bit cleared for each check started
//   cleared track   bits cleared up to the attempts count at the last right password
//
// The count is the first minus the second. The sector is erased only when the attempts track
// is full, which takes thousands of checks.
//
// This file has no hardware in it and is unit tested on the host; core/failflash.c puts it on
// the board's flash.

#ifndef SOS_FAILCOUNT_H
#define SOS_FAILCOUNT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FAILCOUNT_SECTOR 4096
#define FAILCOUNT_PAGE   256
// The two tracks: 7 pages each, after the marker's page. 14336 checks before an erase.
#define FAILCOUNT_TRACK_BYTES (7 * FAILCOUNT_PAGE)
#define FAILCOUNT_CAPACITY    (FAILCOUNT_TRACK_BYTES * 8u)
// When the attempts track is full the sector is erased and the outstanding count written
// back, capped at this: more than enough for the longest delay.
#define FAILCOUNT_CARRY_MAX 64u

typedef struct {
    void *ctx;
    // The sector as it is now, FAILCOUNT_SECTOR bytes. NULL: there is no sector to use (it
    // counts as holding nothing, and the count is then kept in RAM only).
    const uint8_t *(*read)(void *ctx);
    // Program one page (FAILCOUNT_PAGE bytes): bits can go from 1 to 0 only.
    bool (*program)(void *ctx, unsigned page, const uint8_t *data);
    // Erase the sector: every bit 1.
    bool (*erase)(void *ctx);
} failcount_store_t;

// Read the sector. From here on the count is kept in RAM as well and the functions below keep
// the two in step. A sector without the marker counts as zero.
void failcount_init(const failcount_store_t *store);

// Checks started since the last right password.
unsigned failcount_get(void);

// A check is about to start: count it, in flash first. Every write is read back. Returns false
// if it couldn't be written or didn't take (the count in RAM goes up all the same, and never
// down, so the delays still grow until the next reboot).
bool failcount_attempt(void);

// The password was right: the count is zero again. False if that couldn't be written.
bool failcount_clear(void);

#endif
