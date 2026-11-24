// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The device's live configuration (see config.h), kept in flash just below the SSH host key:
//
//   ... firmware ... | config slot A | config slot B | host key |   <- end of flash
//
// With nothing saved (a new board, or after `config reset`) it is empty: no network, no trusted
// keys. The firmware holds no configuration of its own; a board gets one from `setup` on the
// console or from tools/provision.sh, which writes the saved record over USB.

#ifndef SOS_SETTINGS_H
#define SOS_SETTINGS_H

#include <stdbool.h>

#include "core/config.h"

// Load the configuration (or seed it on first boot). Call once at startup, before Wi-Fi runs.
void settings_init(void);

// The live configuration. Edit it, then settings_save().
sos_config_t *settings(void);

// Write the live configuration to flash. False on failure (the previous version stays).
bool settings_save(void);

// Erase the saved configuration: back to the defaults (hostname "sos", user "admin"), with no
// network and no trusted keys.
bool settings_reset(void);

// False if flash can't be used (the firmware image overlaps the config slots).
bool settings_persistent(void);

#endif
