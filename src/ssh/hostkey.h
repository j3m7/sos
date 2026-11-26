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

// Erase the stored host key; a new one is generated on next boot.
bool hostkey_erase(void);

#endif
