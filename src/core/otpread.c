// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See otpread.h. Reads only.

#include "core/otpread.h"

#include "hardware/address_mapped.h"
#include "hardware/regs/addressmap.h"
#include "hardware/regs/otp_data.h"

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

static uint32_t voted(unsigned first_row, unsigned copies, unsigned need) {
    uint32_t rows[8];
    for (unsigned i = 0; i < copies; i++)
        rows[i] = otpread_raw(first_row + i) & 0xffffffu;
    return bootkey_vote(rows, copies, need);
}

void otpread_status(otpread_status_t *st) {
    // CRIT1 is stored eight times and a bit counts when three copies have it; the boot flags
    // are stored three times, two of three (datasheet sections 13.4 and 13.10).
    st->crit1 = voted(OTP_DATA_CRIT1_ROW, 8, 3);
    st->boot_flags0 = voted(OTP_DATA_BOOT_FLAGS0_ROW, 3, 2);
    st->boot_flags1 = voted(OTP_DATA_BOOT_FLAGS1_ROW, 3, 2);
    st->secure_boot = st->crit1 & OTP_DATA_CRIT1_SECURE_BOOT_ENABLE_BITS;
    st->debug_disabled = st->crit1 & OTP_DATA_CRIT1_DEBUG_DISABLE_BITS;
    st->secure_debug_disabled = st->crit1 & OTP_DATA_CRIT1_SECURE_DEBUG_DISABLE_BITS;
    st->glitch_detector = st->crit1 & OTP_DATA_CRIT1_GLITCH_DETECTOR_ENABLE_BITS;
    st->glitch_sensitivity = (st->crit1 & OTP_DATA_CRIT1_GLITCH_DETECTOR_SENS_BITS) >>
                             OTP_DATA_CRIT1_GLITCH_DETECTOR_SENS_LSB;
}

void otpread_bootkeys(bootkey_otp_t *otp) {
    uint32_t flags1 = voted(OTP_DATA_BOOT_FLAGS1_ROW, 3, 2);
    otp->valid = (uint8_t)((flags1 & OTP_DATA_BOOT_FLAGS1_KEY_VALID_BITS) >>
                           OTP_DATA_BOOT_FLAGS1_KEY_VALID_LSB);
    otp->invalid = (uint8_t)((flags1 & OTP_DATA_BOOT_FLAGS1_KEY_INVALID_BITS) >>
                             OTP_DATA_BOOT_FLAGS1_KEY_INVALID_LSB);
    for (unsigned s = 0; s < BOOTKEY_SLOTS; s++) {
        uint16_t rows[BOOTKEY_SLOT_ROWS];
        for (unsigned k = 0; k < BOOTKEY_SLOT_ROWS; k++)
            rows[k] = otpread_ecc(OTP_DATA_BOOTKEY0_0_ROW + s * BOOTKEY_SLOT_ROWS + k);
        bootkey_slot_hash(rows, otp->slot[s]);
    }
}
