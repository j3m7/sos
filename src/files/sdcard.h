// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// microSD card on the PiCowbell Adalogger (exploratory).

#ifndef SOS_SDCARD_H
#define SOS_SDCARD_H

#include <stdbool.h>
#include <stdint.h>

#include "shell/shell.h"

// `sd [info|test [KB [keep]]|speed [MHz]|format [exfat|fat32] [plain]|unmount|check [fix]]`;
// test, speed <MHz>, format, unmount and check need an admin channel. While `sd check` or
// `sd check fix` runs (docs/design/sd-check.md), its question included, the others refuse.
void sdcard_command(shell_t *sh, char *args);

// Mount the card if it isn't yet (initializing the driver first). Says why on failure: to `sh`,
// or in the log when `sh` is NULL. Never while `sd check` runs: false, without touching the card
// (saying so to `sh`).
bool sdcard_mount(shell_t *sh);

// Mount the card if it isn't, quietly (no log lines if there's none): for retrying every so
// often (the log files do). Not after `sd unmount` or `sd check`, until something mounts it with
// sdcard_mount(): the card may be on its way out. Not during `sd check` either. True if a card
// is mounted.
bool sdcard_mount_retry(void);

// Is a card mounted now? Never mounts one.
bool sdcard_mounted(void);
// Changes every time a card is mounted (the same card again too).
uint32_t sdcard_generation(void);

// After the configuration was erased: an encrypted card is unmounted and its key forgotten (the
// owner secret it was opened with no longer exists). A running `sd check` on it stops.
void sdcard_owner_gone(void);

#endif
