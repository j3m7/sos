// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Reading the root secret out of its record. Not in rootsecret.h on purpose: the only code that
// may see the root secret is the key provider's backend (core/rootflash.c), which hands it to
// core/keyprov.c for the length of one derivation. Include this nowhere else (the unit test
// does, to check it).

#ifndef SOS_ROOTSECRET_PRIV_H
#define SOS_ROOTSECRET_PRIV_H

#include "core/rootsecret.h"

// Copy the secret out. False (and `out` zeroed) unless the state is ROOTSECRET_OK. The caller
// wipes `out` after use.
bool rootsecret_read(const rootsecret_store_t *store, uint8_t out[ROOTSECRET_LEN]);

#endif
