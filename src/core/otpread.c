// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See otpread.h. Reads only: the read aliases of the OTP rows, and the CRITICAL status register.

#include "core/otpread.h"

#include <string.h>

#include "hardware/address_mapped.h"
#include "hardware/regs/addressmap.h"
#include "hardware/regs/otp.h"
#include "hardware/regs/otp_data.h"
#include "hardware/structs/otp.h"

// Raw alias: one 32-bit word per row. Error-corrected alias: 16 bits per row, so a 32-bit read
// returns two neighbouring rows, the even one in the low half.
uint32_t otpread_raw(unsigned row) {
    return *(io_ro_32 *)(OTP_DATA_RAW_BASE + 4u * (row & 0xfffu));
}

uint16_t otpread_ecc(unsigned row) {
    row &= 0xfffu;
    uint32_t pair = *(io_ro_32 *)(OTP_DATA_BASE + 4u * (row >> 1));
    return (uint16_t)(row & 1u ? pair >> 16 : pair);
}

uint16_t otpread_ecc_guarded(unsigned row) {
    row &= 0xfffu;
    uint32_t pair = *(io_ro_32 *)(OTP_DATA_GUARDED_BASE + 4u * (row >> 1));
    return (uint16_t)(row & 1u ? pair >> 16 : pair);
}

// A raw row has 24 bits, so all 32 set can only be the "not readable" answer.
#define UNREADABLE 0xffffffffu

// The vote over `copies` raw rows from `first_row`. False if any of them can't be read.
static bool voted(unsigned first_row, unsigned copies, unsigned need, uint32_t *out) {
    uint32_t rows[8];
    *out = 0;
    for (unsigned i = 0; i < copies; i++) {
        rows[i] = otpread_raw(first_row + i);
        if (rows[i] == UNREADABLE)
            return false;
    }
    *out = bootkey_vote(rows, copies, need);
    return true;
}

// The flags of CRIT1 and of the CRITICAL register sit at the same bit positions.
#define CRIT_FLAGS (OTP_CRITICAL_SECURE_BOOT_ENABLE_BITS | OTP_CRITICAL_SECURE_DEBUG_DISABLE_BITS | \
                    OTP_CRITICAL_DEBUG_DISABLE_BITS | OTP_CRITICAL_DEFAULT_ARCHSEL_BITS |           \
                    OTP_CRITICAL_GLITCH_DETECTOR_ENABLE_BITS | OTP_CRITICAL_GLITCH_DETECTOR_SENS_BITS)
_Static_assert(OTP_DATA_CRIT1_SECURE_BOOT_ENABLE_BITS == OTP_CRITICAL_SECURE_BOOT_ENABLE_BITS &&
               OTP_DATA_CRIT1_SECURE_DEBUG_DISABLE_BITS == OTP_CRITICAL_SECURE_DEBUG_DISABLE_BITS &&
               OTP_DATA_CRIT1_DEBUG_DISABLE_BITS == OTP_CRITICAL_DEBUG_DISABLE_BITS &&
               OTP_DATA_CRIT1_GLITCH_DETECTOR_ENABLE_BITS == OTP_CRITICAL_GLITCH_DETECTOR_ENABLE_BITS &&
               OTP_DATA_CRIT1_GLITCH_DETECTOR_SENS_BITS == OTP_CRITICAL_GLITCH_DETECTOR_SENS_BITS,
               "CRIT1 and CRITICAL are compared bit for bit");

void otpread_status(otpread_status_t *st) {
    st->latched = otp_hw->critical;
    st->secure_boot = st->latched & OTP_CRITICAL_SECURE_BOOT_ENABLE_BITS;
    st->debug_disabled = st->latched & OTP_CRITICAL_DEBUG_DISABLE_BITS;
    st->secure_debug_disabled = st->latched & OTP_CRITICAL_SECURE_DEBUG_DISABLE_BITS;
    st->glitch_detector = st->latched & OTP_CRITICAL_GLITCH_DETECTOR_ENABLE_BITS;
    st->glitch_sensitivity = (st->latched & OTP_CRITICAL_GLITCH_DETECTOR_SENS_BITS) >>
                             OTP_CRITICAL_GLITCH_DETECTOR_SENS_LSB;
    // CRIT1 is stored eight times and a bit counts when three copies have it (datasheet
    // section 13.4).
    st->rows_readable = voted(OTP_DATA_CRIT1_ROW, 8, 3, &st->rows);
    st->rows_agree = st->rows_readable && (st->rows & CRIT_FLAGS) == (st->latched & CRIT_FLAGS);
}

bool otpread_bootkeys(bootkey_otp_t *otp) {
    uint32_t flags1;
    memset(otp, 0, sizeof(*otp));
    // The boot flags are stored three times, two of three.
    if (!voted(OTP_DATA_BOOT_FLAGS1_ROW, 3, 2, &flags1))
        return false;
    for (unsigned s = 0; s < BOOTKEY_SLOTS; s++) {
        uint16_t rows[BOOTKEY_SLOT_ROWS];
        for (unsigned k = 0; k < BOOTKEY_SLOT_ROWS; k++) {
            unsigned row = OTP_DATA_BOOTKEY0_0_ROW + s * BOOTKEY_SLOT_ROWS + k;
            // The error-corrected alias answers 0xffff for a row it may not read, which is also
            // a value; the raw alias tells the two apart.
            if (otpread_raw(row) == UNREADABLE) {
                memset(otp, 0, sizeof(*otp));
                return false;
            }
            rows[k] = otpread_ecc(row);
        }
        bootkey_slot_hash(rows, otp->slot[s]);
    }
    otp->valid = (uint8_t)((flags1 & OTP_DATA_BOOT_FLAGS1_KEY_VALID_BITS) >>
                           OTP_DATA_BOOT_FLAGS1_KEY_VALID_LSB);
    otp->invalid = (uint8_t)((flags1 & OTP_DATA_BOOT_FLAGS1_KEY_INVALID_BITS) >>
                             OTP_DATA_BOOT_FLAGS1_KEY_INVALID_LSB);
    return true;
}
