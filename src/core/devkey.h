// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_DEVKEY_H
#define SOS_DEVKEY_H

#include <stdbool.h>
#include <stdint.h>

#define DEVKEY_LEN 32

// The device key: 32 random bytes in the key sector (core/keysector.h). It wraps the data key of
// each encrypted SD card (files/sdcrypt.h), so losing it makes those cards unreadable unless
// their key was saved when `sd format` showed it. It survives reflashing, `config reset` and provisioning.
//
// It's made from the TRNG when the first encrypted card is formatted (devkey_create), not at
// boot, so a board that never encrypts never rewrites the key sector. A page that holds a key
// record is never replaced automatically, even one that isn't valid (damaged, or written by a
// newer firmware): only `sd devkey forget` on the console clears it.

enum {
    DEVKEY_NONE,        // no key yet: devkey_create() makes one
    DEVKEY_OK,
    DEVKEY_INVALID,     // a key record this firmware can't use: left alone
};

int devkey_state(void);

// The key, or NULL unless devkey_state() is DEVKEY_OK.
const uint8_t *devkey(void);

// Make and store a key. Only when the state is DEVKEY_NONE; true if there's a valid key after.
// Writes flash (the whole key sector is rewritten).
bool devkey_create(void);

// Clear the key page (after the user confirmed): cards made with the key can then only be read
// with their saved key.
bool devkey_forget(void);

// At boot: log a key record that can't be used.
void devkey_check(void);

#endif
