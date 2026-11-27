// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The count of wrong console passwords (core/failcount.h) on the board's flash.
//
//   ... firmware ... | wrong-password count | root secret | config A | config B | key sector |
//
// It has a sector of its own, the fifth from the end. Flashing, provisioning, `config reset`
// and `rootsecret forget` leave it alone: none of them is a right password. Someone with a
// cable and the USB bootloader can erase it, as they can everything else in flash.

#ifndef SOS_FAILFLASH_H
#define SOS_FAILFLASH_H

#include <stdbool.h>
#include <stdint.h>

// At boot, before the console starts: read the count. Returns it.
unsigned failflash_init(void);

// False if the firmware image reaches into the sector, so the count can't be kept (it is then
// counted in RAM only, since boot).
bool failflash_usable(void);

// The sector as stored (read through XIP), for the `failcount` health check; NULL if unusable.
const uint8_t *failflash_sector(void);

// For the console's login hooks (shell/login.h): the count now; an answer is about to be
// checked; the answer was right. failflash_clear() is also called when a console password is
// set from the open console.
unsigned failflash_count(void);
void failflash_attempt(void);
void failflash_clear(void);

#endif
