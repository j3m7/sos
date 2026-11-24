// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// microSD card on the PiCowbell Adalogger (exploratory).

#ifndef SOS_SDCARD_H
#define SOS_SDCARD_H

#include "shell.h"

// `sd [info|test [KB [keep]]|speed [MHz]|format [exfat|fat32]|unmount]`; format needs an admin channel.
void sdcard_command(shell_t *sh, char *args);

// Mount the card if it isn't yet (initializing the driver first); prints why on failure.
bool sdcard_mount(shell_t *sh);

#endif
