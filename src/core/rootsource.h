// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Where this board's root secret comes from, decided once at boot, and the key provider
// started on it (core/keyprov.h):
//
//   - OTP, if a secret has been written there (core/roototp.h);
//   - else nothing at all on a console-only board (core/consoleonly.h): its flash is never read
//     for a root secret, and none is made there;
//   - else the flash record of an open board (core/rootflash.h), made at first boot.
//
// With the secret in OTP, keyprov_lock() at the end of boot also locks its OTP page until the
// next reset (the chip's soft lock, a register; nothing is written to OTP), so that a bug
// later on can't read it.
//
// This firmware never writes OTP.

#ifndef SOS_ROOTSOURCE_H
#define SOS_ROOTSOURCE_H

#include <stdbool.h>

#include "core/roototp.h"

// At boot, after consoleonly_init() and wolfCrypt_Init(), before anything derives a key.
void rootsource_init(void);

rootsrc_t rootsource(void);

// Is there a root secret the board can use?
bool rootsource_ok(void);

// One line saying where the root secret is, or why there is none (for `rootsecret`, `version`
// and the locked console). No trailing newline.
const char *rootsource_describe(void);

// What was found in OTP, and the list page it concerns (0-based; ROOTOTP_PAGES if none).
roototp_find_t rootsource_otp_found(void);
unsigned rootsource_otp_index(void);

// Was the OTP page locked for this boot (true only with the secret in OTP, after
// keyprov_lock())?
bool rootsource_otp_locked(void);

// For the `rootsecret` health check: what rootsource_init() saw of a secret it FOUND in OTP
// (it passed its check; the list's pages are locked against the bootloader, else the first
// that isn't). Recorded at boot; nothing is read again.
void rootsource_otp_detail(bool *secret_ok, bool *locks_ok, unsigned *open_page);

// With the secret in OTP: does the chip's soft-lock register for its page say inaccessible to
// Secure and Non-secure code? Reads the register (otp_hw->sw_lock), not the page.
bool rootsource_otp_soft_locked(void);

#endif
