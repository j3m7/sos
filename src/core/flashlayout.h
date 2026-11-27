// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// What SOS keeps at the end of flash, one 4 KB sector each, counted from the end (docs/system.md,
// "Memory layout"). The image grows from the start of flash; every store checks at boot that the
// image ends below its sector, and reads or writes nothing if not.
//
//   -1  keys: the SSH host key (core/keysector.c)
//   -2  configuration slot B (core/settings.c)
//   -3  configuration slot A
//   -4  the root secret, open boards (core/rootflash.c)
//   -5  wrong console passwords (core/failflash.c)
//   -6  reset history and the RTC drift baseline (core/history_board.c)
//
// Older firmware knows only the sectors it had; a new store goes below the lowest, so older
// firmware never reads or writes it. Board code only: it needs the SDK's flash sizes.

#ifndef SOS_FLASHLAYOUT_H
#define SOS_FLASHLAYOUT_H

#include "hardware/flash.h"

#define FLASHLAYOUT_SECTOR_FROM_END(n) (PICO_FLASH_SIZE_BYTES - (n) * FLASH_SECTOR_SIZE)

#define FLASHLAYOUT_KEYS_OFFSET     FLASHLAYOUT_SECTOR_FROM_END(1)
#define FLASHLAYOUT_CONFIG_OFFSET(slot) FLASHLAYOUT_SECTOR_FROM_END(3 - (slot))   // slot 0 = A
#define FLASHLAYOUT_ROOT_OFFSET     FLASHLAYOUT_SECTOR_FROM_END(4)
#define FLASHLAYOUT_FAIL_OFFSET     FLASHLAYOUT_SECTOR_FROM_END(5)
#define FLASHLAYOUT_HISTORY_OFFSET  FLASHLAYOUT_SECTOR_FROM_END(6)

// Each store below the one before it, none overlapping: a change to one offset that broke the
// order fails the build.
_Static_assert(FLASHLAYOUT_CONFIG_OFFSET(1) + FLASH_SECTOR_SIZE <= FLASHLAYOUT_KEYS_OFFSET,
               "config B below the keys");
_Static_assert(FLASHLAYOUT_CONFIG_OFFSET(0) + FLASH_SECTOR_SIZE <= FLASHLAYOUT_CONFIG_OFFSET(1),
               "config A below config B");
_Static_assert(FLASHLAYOUT_ROOT_OFFSET + FLASH_SECTOR_SIZE <= FLASHLAYOUT_CONFIG_OFFSET(0),
               "the root secret below the configuration");
_Static_assert(FLASHLAYOUT_FAIL_OFFSET + FLASH_SECTOR_SIZE <= FLASHLAYOUT_ROOT_OFFSET,
               "the failcount sector below the root secret");
_Static_assert(FLASHLAYOUT_HISTORY_OFFSET + FLASH_SECTOR_SIZE <= FLASHLAYOUT_FAIL_OFFSET,
               "the history sector below the failcount sector");
_Static_assert(FLASHLAYOUT_HISTORY_OFFSET % FLASH_SECTOR_SIZE == 0, "sectors are aligned");

#endif
