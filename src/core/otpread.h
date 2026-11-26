// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Reading the RP2350's one-time-programmable (OTP) memory. READING ONLY, and by a path that
// cannot write: the chip's memory-mapped read aliases (datasheet section 13, "read data
// region"), to which a write is a bus fault, and one read-only status register (CRITICAL).
// Nothing here calls the boot ROM's otp_access function, which is the one that can program
// OTP. Everyday firmware carries no code that writes OTP (docs/securing-a-board.md).
//
// These are the unguarded aliases: a row this code isn't allowed to read comes back as all
// ones instead of a fault, which suits a status display. They are NOT for reading a secret:
// that needs the guarded aliases, which fault on a failed check.

#ifndef SOS_OTPREAD_H
#define SOS_OTPREAD_H

#include <stdbool.h>
#include <stdint.h>

#include "core/bootkey.h"

// A row's 24 raw bits. 0xffffffff when it can't be read (no permission).
uint32_t otpread_raw(unsigned row);

// A row's 16 data bits after error correction. 0xffff when it can't be read, which is also a
// possible value.
uint16_t otpread_ecc(unsigned row);

// The flags that say how the board is secured.
//
// They come from the chip's CRITICAL register: what the chip itself read from OTP at reset and
// acts on (it is what gates secure boot and the debug port). The same flags are also read from
// their OTP rows (CRIT1, stored eight times, by vote) as a cross-check. A row that can't be
// read is all ones, which would otherwise look like "everything set": it is reported as
// unreadable instead, never as a state.
typedef struct {
    bool secure_boot;                   // SECURE_BOOT_ENABLE
    bool debug_disabled;                // DEBUG_DISABLE
    bool secure_debug_disabled;         // SECURE_DEBUG_DISABLE
    bool glitch_detector;               // GLITCH_DETECTOR_ENABLE
    unsigned glitch_sensitivity;        // GLITCH_DETECTOR_SENS, 0..3
    uint32_t latched;                   // the CRITICAL register
    bool rows_readable;                 // the CRIT1 rows could be read
    uint32_t rows;                      // their voted value (meaningless if not readable)
    bool rows_agree;                    // readable, and the same flags as the register
} otpread_status_t;

void otpread_status(otpread_status_t *st);

// The four boot key slots and their valid / invalid marks. Returns false, with everything
// zeroed, if any of those rows can't be read: nothing may then be concluded from it.
bool otpread_bootkeys(bootkey_otp_t *otp);

#endif
