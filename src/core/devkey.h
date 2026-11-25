// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_DEVKEY_H
#define SOS_DEVKEY_H

#include <stdbool.h>
#include <stdint.h>

#define DEVKEY_LEN 32

// The device key: 32 random bytes in the key sector (core/keysector.h), made from the TRNG on
// first boot. It wraps the data key of each encrypted SD card (files/sdcrypt.h), so losing it
// makes those cards unreadable unless their key was saved with `sd key`. It survives
// reflashing, `config reset` and provisioning, and a valid one is never replaced.
// Call at boot, after wolfCrypt_Init() and before Wi-Fi (it may write flash).
bool devkey_init(void);

// The key, or NULL if there is none (devkey_init failed).
const uint8_t *devkey(void);

#endif
