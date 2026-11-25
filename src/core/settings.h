// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The device's live configuration (see config.h), kept in flash just below the key sector (the SSH host
// key), and above the root secret (core/rootflash.h):
//
//   ... firmware ... | root secret | config slot A | config slot B | key sector |   <- end of flash
//
// It's stored sealed: AES-256-GCM under a key derived from the board's root secret
// (core/seal.h, core/keyprov.h), so the flash contents don't give away the Wi-Fi password or let
// someone plant a trusted key. The seal key is held in RAM by settings.c while the board runs.
// A plain record (older firmware, provisioning) is loaded on an open board and sealed at once.
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

// Write the live configuration to flash. False on failure (the previous version stays). If the
// configuration has no owner secret yet, one is made first (see config.h): at the first save of
// a new board, when a plain record is sealed, and after `config reset`.
bool settings_save(void);

// Does the configuration have an owner secret (to encrypt and open SD cards with)?
bool settings_has_owner_secret(void);

// Erase the saved configuration: back to the defaults (hostname "sos", user "admin"), with no
// network and no trusted keys. The owner secret goes with it: SD cards this board encrypted can
// then only be read with the key shown when each was formatted.
bool settings_reset(void);

// False if flash can't be used (the firmware image overlaps the config slots).
bool settings_persistent(void);

#endif
