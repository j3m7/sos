// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The key this board wraps its encrypted SD cards' data keys with (files/sdcrypt.h). It depends
// on two things (issue #64):
//
//   card key = HMAC-SHA256(key = wrap base key, message = owner secret)
//
// - The wrap base key comes from the board's root secret (KEYPROV_SD_WRAP_BASE). It's derived
//   once at boot, before the key provider locks, and kept here in RAM while the board runs.
// - The owner secret is 32 random bytes in the sealed configuration (core/config.h). It's made
//   when the board is first set up, and destroyed when the configuration is erased.
//
// So erasing the configuration (`config reset`, or over USB) locks every card this board
// encrypted: the board that comes back up is a new owner, and only the key shown when a card
// was formatted reads it. Without that, someone who erased the configuration could set the
// board up as their own and have it unlock the cards.
//
// The card key itself is computed when a card is opened or formatted and wiped afterwards.

#ifndef SOS_CARDKEY_H
#define SOS_CARDKEY_H

#include <stdbool.h>
#include <stdint.h>

#define CARDKEY_LEN 32

// At boot, before keyprov_lock(): derive the wrap base key.
void cardkey_init(void);

// False if there's no usable root secret: this board can't encrypt or open cards at all.
bool cardkey_possible(void);

// The card key for the current owner secret. False (out zeroed) if cards can't be opened: no
// root secret, or no owner secret yet. The caller wipes `out`.
bool cardkey_get(uint8_t out[CARDKEY_LEN]);

// Before formatting an encrypted card: make sure there is an owner secret, making one and
// saving the configuration if there wasn't. False if that isn't possible.
bool cardkey_ensure(void);

#endif
