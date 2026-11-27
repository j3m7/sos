// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_HOSTKEY_H
#define SOS_HOSTKEY_H

#include <stdbool.h>
#include <stdint.h>

// At boot, once, after the key provider has started and before it locks: derive the base key
// the host key's seal is made from.
void hostkey_init(void);

// The device's SSH host key (Ed25519, DER), from the key sector, making and storing a new one
// if there is none this board's owner can open (ssh/hostkey_record.h). The private key never
// leaves the device. It needs the configuration loaded (the owner secret is in it).
//
// False if there is no host key. On a board that isn't set up yet that is the normal state:
// call again once it is (cheap while nothing changed). Any other failure is final until the
// next reboot. The first call should come before Wi-Fi starts, when a flash write is simplest.
bool hostkey_get(const uint8_t **der, uint32_t *len);

// The owner secret was just destroyed (`config reset`): the host key in RAM belonged to that
// owner and must not be used again. Forgets it, so that the next hostkey_get() decides afresh
// (none while the board isn't set up; then a new key for the new owner, since the stored
// record no longer opens). THE SSH SERVER MUST BE STOPPED FIRST (sshd_stop): it holds a
// pointer to the key.
void hostkey_owner_gone(void);

// Erase the stored host key; a new one is generated on next boot.
bool hostkey_erase(void);

// For the `hostkey` health check (core/statecheck.h). The record is never opened for this:
// page_matches compares a SHA-256 of the stored page (the sealed record, as stored) with the one
// taken when the key in use was opened or stored.
typedef struct {
    bool have_key;              // a host key is in use
    bool gave_up;               // none, for a reason that lasts until a reboot (or a refusal)
    bool unsealed;              // the key in use is a plain record, used as it is
    bool page_matches;          // have_key, and the stored page is the one loaded
    bool erased;                // hostkey_erase() since then: the page is gone on purpose
} hostkey_health_t;
void hostkey_health(hostkey_health_t *out);

#endif
