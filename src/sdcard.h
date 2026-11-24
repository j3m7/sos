// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// microSD card on the PiCowbell Adalogger (exploratory).

#ifndef SOS_SDCARD_H
#define SOS_SDCARD_H

#include "shell.h"

// `sd [info|ls [path]|test [KB]|speed [MHz]|format [exfat|fat32]|unmount]`; format needs an admin channel.
void sdcard_command(shell_t *sh, char *args);

#endif
