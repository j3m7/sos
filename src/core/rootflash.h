// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The root secret of an open board, in flash, and the key provider's backend that reads it
// (core/rootsecret.h, core/keyprov.h).
//
//   ... firmware ... | password count | root secret | config slot A | config slot B | key sector |
//                                                                                (end of flash)
//
// It has a sector of its own, the fourth from the end, and not a page of the key sector:
// firmware from before the root secret rewrites the key sector with only the pages it knows
// (when it makes a host key), which would erase it. Nothing but this file writes the sector:
// flashing, provisioning and `config reset` leave it alone. Its first two pages are programmed
// once, when the secret is made; the sector is erased only by `rootsecret forget` (or to start
// over a first write that was cut off before any secret was used).

#ifndef SOS_ROOTFLASH_H
#define SOS_ROOTFLASH_H

#include <stdbool.h>

// At boot, once, after wolfCrypt_Init() and before Wi-Fi: make the root secret if there's
// none, log what was found, and start the key provider on it. Returns the state
// (ROOTSECRET_OK, or why there's no usable secret).
int rootflash_init(void);

// The state now (ROOTSECRET_*), and whether one of the two copies is damaged.
int rootflash_state(void);
bool rootflash_degraded(void);

// False if the firmware image reaches into the sector, so it can't be used.
bool rootflash_usable(void);

// Erase the sector (after the user confirmed); the next boot makes a new secret.
bool rootflash_forget(void);

#endif
