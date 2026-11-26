// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "ssh/hostkey.h"

#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "pico/stdlib.h"

#include <wolfssh/ssh.h>
#include <wolfssh/keygen.h>

#include "core/keyprov.h"
#include "core/keysector.h"
#include "core/log.h"
#include "core/random.h"
#include "core/seal.h"
#include "core/sealedonly.h"
#include "ssh/hostkey_record.h"

// The host key is the first page of the key sector (core/keysector.h), which survives
// reflashing. It's stored sealed (ssh/hostkey_record.h) under a key derived from the root
// secret; the key in use is kept here in RAM while the board runs, since the SSH server needs
// it for every connection.

_Static_assert(HOSTREC_PAGE == FLASH_PAGE_SIZE, "the record is one flash page");

// An open board takes over a plain host key from older firmware and seals it. A sealed-only
// board must not (core/sealedonly.h), like the configuration's refuse_plain (core/settings.c).
#define refuse_plain sealedonly()

static uint8_t key_der[HOSTREC_PAGE];       // the private key in use
static uint8_t page[HOSTREC_PAGE];          // a record being built

static void wipe(void *p, size_t len) {
    volatile uint8_t *v = p;
    while (len--)
        *v++ = 0;
}

// Seal key_der[0..len) and store it. The key sector is erased and rewritten for this
// (keysector_write), as for any change to it.
static bool store_sealed(const sealer_t *sealer, uint32_t len) {
    bool ok = hostrec_seal(page, sealer, key_der, len) &&
              keysector_write(KEYSECTOR_HOSTKEY, page) &&
              hostrec_kind(keysector_page(KEYSECTOR_HOSTKEY)) == HOSTREC_SEALED;
    wipe(page, sizeof(page));
    return ok;
}

bool hostkey_get(const uint8_t **der, uint32_t *len) {
    const uint8_t *stored = keysector_page(KEYSECTOR_HOSTKEY);
    // The seal key is needed only here, at boot: derived, used and wiped.
    seal_ctx_t seal = { .random = random_bytes };
    seal.have_key = keyprov_derive(KEYPROV_HOSTKEY_SEAL, seal.key);
    sealer_t sealer = seal_sealer(&seal);
    bool ok = false;
    *len = 0;

    int kind = hostrec_kind(stored);
    if (kind == HOSTREC_PLAIN && hostrec_open(stored, NULL, refuse_plain, key_der, len)) {
        // From firmware before sealing: keep the key (and its fingerprint), store it sealed.
        ok = true;
        if (!seal.have_key)
            log_msg(SOS_LOG_ERR, "hostkey: the host key is stored unsealed and there's no key to "
                                 "seal it with (see `rootsecret`); using it as it is");
        else if (store_sealed(&sealer, *len))
            log_msg(SOS_LOG_NOTICE, "hostkey: sealed the stored host key (same key, same "
                                    "fingerprint)");
        else
            log_msg(SOS_LOG_ERR, "hostkey: sealing the stored host key failed; it stays unsealed");
    } else if (!seal.have_key) {
        // Without the root secret a sealed key can't be opened and a new one couldn't be
        // sealed. No host key, so no SSH, until that's put right.
        log_msg(SOS_LOG_ERR, "hostkey: no host key: there's no key to %s (see `rootsecret`)",
                kind == HOSTREC_SEALED ? "open the stored one with" : "seal a new one with");
    } else if (kind == HOSTREC_SEALED &&
               hostrec_open(stored, &sealer, refuse_plain, key_der, len)) {
        ok = true;
    } else {
        log_msg(SOS_LOG_NOTICE, kind == HOSTREC_SEALED
                    ? "hostkey: the stored host key doesn't open (sealed under another root "
                      "secret, or damaged); making a NEW host key: SSH clients will see a "
                      "changed fingerprint"
                    : kind == HOSTREC_PLAIN
                          ? "hostkey: the stored host key is unsealed and isn't used; making a "
                            "new host key"
                          : "hostkey: no valid host key in flash, generating Ed25519 key...");
        int n = wolfSSH_MakeEd25519Key(key_der, sizeof(key_der), WOLFSSH_ED25519KEY);
        if (n <= 0 || n > HOSTREC_MAX_KEY) {
            log_msg(SOS_LOG_ERR, "hostkey: key generation failed (%d)", n);
        } else if (!store_sealed(&sealer, (uint32_t)n)) {
            log_msg(SOS_LOG_ERR, "hostkey: failed to store host key in flash");
        } else {
            *len = (uint32_t)n;
            ok = true;
            log_msg(SOS_LOG_NOTICE, "hostkey: stored new host key (%lu bytes, sealed)",
                    (unsigned long)*len);
        }
    }
    seal_wipe(&seal);
    if (!ok) {
        wipe(key_der, sizeof(key_der));
        *len = 0;
    }
    *der = key_der;
    return ok;
}

bool hostkey_erase(void) {
    return keysector_write(KEYSECTOR_HOSTKEY, NULL);      // keeps the device key
}
