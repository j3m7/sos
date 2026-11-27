// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// microSD card on the PiCowbell Adalogger (exploratory).

#ifndef SOS_SDCARD_H
#define SOS_SDCARD_H

#include <stdbool.h>
#include <stdint.h>

#include "shell/shell.h"

// `sd [info|test [KB [keep]]|speed [MHz]|format [exfat|fat32] [plain]|unmount]`;
// test, speed <MHz>, format and unmount need an admin channel.
void sdcard_command(shell_t *sh, char *args);

// Mount the card if it isn't yet (initializing the driver first). Says why on failure: to `sh`,
// or in the log when `sh` is NULL.
bool sdcard_mount(shell_t *sh);

// Is a card mounted now? Never mounts one.
bool sdcard_mounted(void);
// Changes every time a card is mounted (the same card again too).
uint32_t sdcard_generation(void);

// After the configuration was erased: an encrypted card is unmounted and its key forgotten (the
// owner secret it was opened with no longer exists).
void sdcard_owner_gone(void);

#endif
