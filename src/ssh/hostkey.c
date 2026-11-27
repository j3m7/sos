// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "ssh/hostkey.h"

#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "pico/stdlib.h"

#include <wolfssh/ssh.h>
#include <wolfssh/keygen.h>

#include "core/health.h"
#include "core/keyprov.h"
#include "core/keysector.h"
#include "core/log.h"
#include "core/random.h"
#include "core/seal.h"
#include "core/consoleonly.h"
#include "core/settings.h"
#include "ssh/hostkey_record.h"

// The host key is the first page of the key sector (core/keysector.h), which survives
// reflashing. It's stored sealed (ssh/hostkey_record.h) under a key made from two things: a
// base key derived from the board's root secret, and the owner secret in the configuration.
// Erasing the configuration therefore makes the stored host key unreadable, and the next
// setup makes a new one.
//
// The base key is derived once at boot, before the key provider locks, and kept here in RAM:
// the host key may have to be made later, when a board that started without a configuration
// is set up. The key in use is kept in RAM too, since the SSH server needs it for every
// connection.

_Static_assert(HOSTREC_PAGE == FLASH_PAGE_SIZE, "the record is one flash page");
_Static_assert(HOSTREC_KEY_LEN == KEYPROV_KEY_LEN && HOSTREC_KEY_LEN == SEAL_KEY_LEN,
               "the base key, the owner key and the seal key are the same size");

static uint8_t base[HOSTREC_KEY_LEN];       // from the root secret; zero if there is none
static bool have_base;
static uint8_t key_der[HOSTREC_PAGE];       // the private key in use
static uint32_t key_len;
static bool have_key;
static bool gave_up;                        // it failed for a reason that won't change before a reboot
static bool refused;                        // ...or because a health check refused a new key
static bool said_no_owner;
static uint8_t page[HOSTREC_PAGE];          // a record being built

static void wipe(void *p, size_t len) {
    volatile uint8_t *v = p;
    while (len--)
        *v++ = 0;
}

void hostkey_init(void) {
    have_base = keyprov_derive(KEYPROV_HOSTKEY_SEAL, base);
}

// Seal key_der[0..len) under the owner's key and store it. The key sector is erased and
// rewritten for this (keysector_write), as for any change to it.
static bool store_sealed(const sealer_t *sealer, uint32_t len) {
    bool ok = hostrec_seal(page, sealer, key_der, len) &&
              keysector_write(KEYSECTOR_HOSTKEY, page) &&
              hostrec_kind(keysector_page(KEYSECTOR_HOSTKEY)) == HOSTREC_SEALED;
    wipe(page, sizeof(page));
    return ok;
}

static bool make_new(const sealer_t *sealer, uint32_t *len) {
    // A new key is random numbers: refused while a check holding HEALTH_GATE_KEYS fails. No host
    // key means no sshd; the console still works.
    const char *held = health_gate_refuse(HEALTH_GATE_KEYS);
    if (held) {
        log_msg(SOS_LOG_AUTH | SOS_LOG_ERR, "hostkey: no new host key made: health check %s failed",
                held);
        refused = true;
        return false;
    }
    int n = wolfSSH_MakeEd25519Key(key_der, sizeof(key_der), WOLFSSH_ED25519KEY);
    if (n <= 0 || n > HOSTREC_MAX_KEY) {
        log_msg(SOS_LOG_AUTH | SOS_LOG_ERR, "hostkey: key generation failed (%d)", n);
        return false;
    }
    if (!store_sealed(sealer, (uint32_t)n)) {
        log_msg(SOS_LOG_AUTH | SOS_LOG_ERR, "hostkey: failed to store host key in flash");
        return false;
    }
    *len = (uint32_t)n;
    log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE,
            "hostkey: stored new host key (%lu bytes, sealed for this owner)",
            (unsigned long)*len);
    return true;
}

bool hostkey_get(const uint8_t **der, uint32_t *len) {
    *der = key_der;
    *len = have_key ? key_len : 0;
    if (have_key)
        return true;
    // A refusal lasts only as long as the check fails (the latest result counts): asked again
    // once the gate is open.
    if (gave_up && refused && !health_gate(HEALTH_GATE_KEYS))
        gave_up = refused = false;
    if (gave_up)
        return false;

    const uint8_t *stored = keysector_page(KEYSECTOR_HOSTKEY);
    int kind = hostrec_kind(stored);
    bool have_owner = settings_has_owner_secret();
    hostkey_action_t action = hostrec_decide(kind, have_base, have_owner, consoleonly());

    if (action == HOSTKEY_NONE_NO_OWNER) {
        // Not an error and not final: the board hasn't been set up. Asked again later.
        if (!said_no_owner)
            log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE,
                    "hostkey: no host key yet: this board isn't set up, and its "
                    "host key belongs to its owner (it is made at setup)");
        said_no_owner = true;
        return false;
    }

    // The two seal keys, needed only here: the owner's (base and owner secret), and the base
    // alone for a record from older firmware. Used and wiped.
    seal_ctx_t owner_seal = { .random = random_bytes }, old_seal = { .random = random_bytes };
    if (have_base && have_owner)
        owner_seal.have_key = hostrec_owner_key(owner_seal.key, base, settings()->owner_secret);
    if (have_base) {
        memcpy(old_seal.key, base, sizeof(base));
        old_seal.have_key = true;
    }
    sealer_t sealer = seal_sealer(&owner_seal), old_sealer = seal_sealer(&old_seal);
    uint32_t n = 0;
    bool ok = false;

    switch (action) {
    case HOSTKEY_OPEN:
        ok = owner_seal.have_key && hostrec_open(stored, &sealer, true, key_der, &n);
        if (!ok && owner_seal.have_key) {
            log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE,
                    "hostkey: the stored host key doesn't open (it belongs to an "
                    "earlier owner or root secret, or is damaged); making a NEW "
                    "host key: SSH clients will see a changed fingerprint");
            ok = make_new(&sealer, &n);
        }
        break;
    case HOSTKEY_TAKE_OVER_OLD:
        if (owner_seal.have_key && hostrec_open(stored, &old_sealer, true, key_der, &n)) {
            // From firmware before host keys were tied to the owner: keep the key (and its
            // fingerprint) and store it the new way.
            ok = true;
            if (store_sealed(&sealer, n))
                log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE,
                        "hostkey: the stored host key is now sealed for this "
                        "owner (same key, same fingerprint)");
            else
                log_msg(SOS_LOG_AUTH | SOS_LOG_ERR,
                        "hostkey: sealing the stored host key for this owner "
                        "failed; it stays as it was");
        } else if (owner_seal.have_key) {
            log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE,
                    "hostkey: the stored host key doesn't open (sealed under "
                    "another root secret, or damaged); making a NEW host key: "
                    "SSH clients will see a changed fingerprint");
            ok = make_new(&sealer, &n);
        }
        break;
    case HOSTKEY_TAKE_OVER_PLAIN:
        if (owner_seal.have_key && hostrec_open(stored, NULL, false, key_der, &n)) {
            ok = true;
            if (store_sealed(&sealer, n))
                log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE,
                        "hostkey: sealed the stored host key (same key, same "
                        "fingerprint)");
            else
                log_msg(SOS_LOG_AUTH | SOS_LOG_ERR,
                        "hostkey: sealing the stored host key failed; it stays "
                        "unsealed");
        }
        break;
    case HOSTKEY_MAKE_NEW:
        if (owner_seal.have_key) {
            log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, kind == HOSTREC_SEALED_OLD
                        ? "hostkey: the stored host key isn't tied to an owner, and a "
                          "console-only board doesn't carry it over; making a new host key"
                        : kind == HOSTREC_PLAIN
                              ? "hostkey: the stored host key is unsealed and isn't used; making "
                                "a new host key"
                              : "hostkey: no valid host key in flash, generating Ed25519 key...");
            ok = make_new(&sealer, &n);
        }
        break;
    case HOSTKEY_USE_PLAIN_UNSEALED:
        ok = hostrec_open(stored, NULL, false, key_der, &n);
        log_msg(SOS_LOG_AUTH | SOS_LOG_ERR,
                "hostkey: the host key is stored unsealed and there's no key to "
                "seal it with (see `rootsecret`); using it as it is");
        break;
    case HOSTKEY_NONE_NO_KEYS:
        // Without the root secret a sealed key can't be opened and a new one couldn't be
        // sealed. No host key, so no SSH, until that's put right.
        log_msg(SOS_LOG_AUTH | SOS_LOG_ERR,
                "hostkey: no host key: there's no key to %s (see `rootsecret`)",
                kind == HOSTREC_SEALED || kind == HOSTREC_SEALED_OLD ? "open the stored one with"
                                                                      : "seal a new one with");
        break;
    case HOSTKEY_NONE_NO_OWNER:
        break;                          // handled above
    }
    if (have_base && have_owner && !owner_seal.have_key)
        log_msg(SOS_LOG_AUTH | SOS_LOG_ERR,
                "hostkey: making the key that seals the host key failed");

    seal_wipe(&owner_seal);
    seal_wipe(&old_seal);
    if (ok) {
        have_key = true;
        key_len = n;
    } else {
        wipe(key_der, sizeof(key_der));
        gave_up = true;                 // a reboot tries again
    }
    *len = have_key ? key_len : 0;
    return ok;
}

void hostkey_owner_gone(void) {
    wipe(key_der, sizeof(key_der));
    key_len = 0;
    have_key = false;
    gave_up = false;                    // what failed for the old owner may not for the next
    refused = false;
    said_no_owner = false;
}

bool hostkey_erase(void) {
    return keysector_write(KEYSECTOR_HOSTKEY, NULL);      // keeps the device key
}
