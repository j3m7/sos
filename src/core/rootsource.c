// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See rootsource.h.

#include "core/rootsource.h"

#include <stdio.h>

#include "hardware/structs/otp.h"

#include "core/keyprov.h"
#include "core/log.h"
#include "core/otpread.h"
#include "core/rootflash.h"
#include "core/rootsecret.h"
#include "core/sealedonly.h"

static rootsrc_t source = ROOTSRC_NONE;
static roototp_find_t found = ROOTOTP_CANT_READ;
static unsigned otp_index = ROOTOTP_PAGES;
static bool otp_locked;
static char why[160] = "not started";

// --- reading OTP -------------------------------------------------------------------------------------

static uint32_t rd_raw(void *ctx, unsigned row) { (void)ctx; return otpread_raw(row); }
static uint16_t rd_ecc(void *ctx, unsigned row) { (void)ctx; return otpread_ecc(row); }
static bool rd_careful(void *ctx, unsigned row, unsigned count, uint16_t *out) {
    (void)ctx;
    for (unsigned i = 0; i < count; i++)
        out[i] = otpread_ecc_guarded(row + i);
    return true;
}
static const roototp_reader_t reader = { NULL, rd_raw, rd_ecc, rd_careful };

// --- the key provider's backend for a secret in OTP ---------------------------------------------------

_Static_assert(KEYPROV_ROOT_LEN == ROOTOTP_SECRET_LEN, "the provider's root is the stored secret");

static bool otp_root(void *ctx, uint8_t out[KEYPROV_ROOT_LEN]) {
    (void)ctx;
    return !otp_locked && roototp_read(&reader, otp_index, out);
}

// The chip's soft lock for the page: Secure and Non-secure access both to "inaccessible".
// It is a register, set until the next reset; it can be raised but not lowered. This is not a
// write to OTP memory.
static void otp_lock(void *ctx) {
    (void)ctx;
    unsigned page = ROOTOTP_FIRST_PAGE + otp_index;
    otp_hw->sw_lock[page] = OTP_SW_LOCK0_SEC_BITS | OTP_SW_LOCK0_NSEC_BITS;
    otp_locked = true;
    // It must now read as unreadable. If it doesn't, say so loudly: the lock is what keeps a
    // later bug from reading the secret.
    if (otpread_raw(ROOTOTP_ROW(otp_index, 0)) != 0xffffffffu)
        log_msg(SOS_LOG_ERR, "rootsecret: LOCKING OTP PAGE %u DID NOT TAKE: the root secret is "
                             "still readable by this firmware", page);
    else
        log_msg(SOS_LOG_NOTICE, "rootsecret: OTP page %u is locked until the next reset", page);
}

static const keyprov_backend_t otp_backend = { otp_root, otp_lock, NULL };

// --- no source ------------------------------------------------------------------------------------------

static bool no_root(void *ctx, uint8_t out[KEYPROV_ROOT_LEN]) {
    (void)ctx;
    (void)out;
    return false;
}
static const keyprov_backend_t no_backend = { no_root, NULL, NULL };

// --- public ---------------------------------------------------------------------------------------------

void rootsource_init(void) {
    uint8_t probe[ROOTOTP_SECRET_LEN];
    bool secret_ok = false;
    found = roototp_find(&reader, &otp_index);
    if (found == ROOTOTP_FOUND) {
        // Read it once now to know that it can be used; it is read again for each derivation
        // and never kept.
        secret_ok = roototp_read(&reader, otp_index, probe);
        keyprov_wipe(probe, sizeof(probe));
    }
    source = roototp_choose(found, secret_ok, sealedonly());
    unsigned page = ROOTOTP_FIRST_PAGE + otp_index;

    switch (source) {
    case ROOTSRC_OTP:
        keyprov_init(&otp_backend);
        snprintf(why, sizeof(why), "present, in OTP page %u", page);
        log_msg(SOS_LOG_NOTICE, "rootsecret: the root secret is in OTP page %u", page);
        return;
    case ROOTSRC_FLASH: {
        if (found == ROOTOTP_EXHAUSTED)
            log_msg(SOS_LOG_WARNING, "rootsecret: every OTP page for the root secret is spent; "
                                     "this open board uses its flash record");
        // An open board: the flash record, made now if there is none. It starts the provider.
        int state = rootflash_init();
        snprintf(why, sizeof(why), "%s",
                 !rootflash_usable() ? "none (the firmware image overlaps its flash sector)"
                 : state == ROOTSECRET_OK ? "present, in flash (open board: it isn't protected "
                                            "from someone who has the board)"
                 : state == ROOTSECRET_NONE ? "none (making it at boot failed, see dmesg; `reboot` "
                                              "tries again)"
                 : "present in flash but unusable (damaged, or written by newer firmware)");
        return;
    }
    case ROOTSRC_NONE:
        break;
    }

    keyprov_init(&no_backend);
    if (found == ROOTOTP_FOUND)
        snprintf(why, sizeof(why), "none: OTP page %u holds a root secret that fails its check "
                                   "(damaged, or the read was disturbed)", page);
    else if (found == ROOTOTP_CANT_READ)
        snprintf(why, sizeof(why), "none: OTP page %u, where it would be, can't be read", page);
    else if (found == ROOTOTP_EXHAUSTED)
        snprintf(why, sizeof(why), "none: this is a sealed-only board and every OTP page for "
                                   "the root secret is spent");
    else
        snprintf(why, sizeof(why), "none: this is a sealed-only board and no root secret has "
                                   "been written to its OTP (its flash is not used for one)");
    log_msg(SOS_LOG_ERR, "rootsecret: %s; there are no keys", why);
}

rootsrc_t rootsource(void) { return source; }

bool rootsource_ok(void) {
    return source == ROOTSRC_OTP ||
           (source == ROOTSRC_FLASH && rootflash_state() == ROOTSECRET_OK);
}

const char *rootsource_describe(void) { return why; }
roototp_find_t rootsource_otp_found(void) { return found; }
unsigned rootsource_otp_index(void) { return otp_index; }
bool rootsource_otp_locked(void) { return otp_locked; }
