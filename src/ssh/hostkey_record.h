// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The SSH host key's record in the key sector (one 256-byte flash page). Pure C, host tested;
// ssh/hostkey.c does the flash and the key generation.
//
// Sealed record, written by this firmware (multi-byte numbers little-endian):
//    0  magic "SOSK"        4   (0x4b534f53)
//    4  version (3)         4
//    8  key length n        4   (the private key, DER; at most HOSTREC_MAX_KEY)
//   12  the key, sealed     n + 28: nonce (12), ciphertext (n), tag (16)   (core/sealer.h)
//       erased (0xff) to the end of the page
// The associated data of the seal is the first 12 bytes, so the sealed key can't be put under
// another length or version.
//
// Whose key seals it. Version 3 is sealed under a key that depends on the board AND on its
// owner:
//
//   seal key = HMAC-SHA256(key = base key, message = owner secret)
//
// The base key comes from the board's root secret (KEYPROV_HOSTKEY_SEAL); the owner secret is
// in the sealed configuration and is destroyed when the configuration is erased
// (core/config.h), exactly as for the key of encrypted SD cards (files/cardkey.h). So erasing
// a board's configuration makes its host key unreadable, and whoever sets the board up next
// gets a new one: a new owner is a new SSH identity. Without that, someone who wiped a board
// and set it up as theirs would answer SSH with the old host key, and the previous owner's
// clients would trust it.
//
// Version 2, written by firmware before that, is sealed under the base key alone, so it
// survives a wipe. An open board takes it over once (same key, same fingerprint) and stores it
// as version 3. A sealed-only board doesn't use it.
//
// Plain record, written by firmware from before sealing (still read, so an upgrade keeps the
// board's host key and fingerprint; hostkey.c then seals it):
//    0  magic "SOSK"   4
//    4  version (1)    4
//    8  key length n   4   (at most 240)
//   12  check          4   FNV-1a over the key
//   16  the key        n

#ifndef SOS_HOSTKEY_RECORD_H
#define SOS_HOSTKEY_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/sealer.h"

#define HOSTREC_PAGE    256
#define HOSTREC_MAX_KEY (HOSTREC_PAGE - 12 - SEALER_OVERHEAD)       // 216

enum {
    HOSTREC_NONE,           // not a host key record this firmware can use (blank, damaged, unknown)
    HOSTREC_PLAIN,          // a valid plain record
    HOSTREC_SEALED,         // a sealed record, version 3: under the board's and the owner's key
    HOSTREC_SEALED_OLD,     // a sealed record, version 2: under the board's key alone
};
// (Whether a sealed record opens is hostrec_open's answer.)

// What the page holds, from its header alone (and the check, for a plain record).
int hostrec_kind(const uint8_t page[HOSTREC_PAGE]);

// Copy the key out of the page into der (HOSTREC_PAGE bytes of room) and set *len. A sealed
// record of either version is opened with `sealer` (NULL: it can't be); the caller gives the
// sealer that goes with the record's kind. With refuse_plain a plain record isn't
// used: for a secured board, where a plain record is one that someone who can write the flash
// may have planted, with a key they know. False, with der zeroed, if the page holds no usable
// record or the sealed one doesn't open.
bool hostrec_open(const uint8_t page[HOSTREC_PAGE], const sealer_t *sealer, bool refuse_plain,
                  uint8_t *der, uint32_t *len);

// Build a sealed record (version 3) for a key in `page`. False (page zeroed) if the key is too
// long or sealing failed.
bool hostrec_seal(uint8_t page[HOSTREC_PAGE], const sealer_t *sealer, const uint8_t *der,
                  uint32_t len);

// The key version 3 is sealed under, from the base key and the owner secret. False (out
// zeroed) on failure. The caller wipes `out`.
#define HOSTREC_KEY_LEN 32
bool hostrec_owner_key(uint8_t out[HOSTREC_KEY_LEN], const uint8_t base[HOSTREC_KEY_LEN],
                       const uint8_t owner[32]);

// What to do about the host key, given what is stored and what the board has. The decision is
// here, apart from the flash and the key generation, so that it can be tested.
typedef enum {
    HOSTKEY_OPEN,               // open the version 3 record; if it doesn't open, make a new key
    HOSTKEY_TAKE_OVER_OLD,      // open the version 2 record under the base key and store it as
                                // version 3; if it doesn't open, make a new key
    HOSTKEY_TAKE_OVER_PLAIN,    // use the plain record's key and store it as version 3
    HOSTKEY_MAKE_NEW,           // make a new key and store it as version 3
    HOSTKEY_USE_PLAIN_UNSEALED, // no root secret on an open board: use the plain key as it is
    HOSTKEY_NONE_NO_KEYS,       // no root secret: there is no host key
    HOSTKEY_NONE_NO_OWNER,      // the board isn't set up yet (no owner secret): no host key yet,
                                // and nothing stored is touched
} hostkey_action_t;

// `kind`: hostrec_kind() of what is stored. `have_base`: the base key could be derived.
// `have_owner`: the configuration has an owner secret. `sealed_only`: core/sealedonly.h.
hostkey_action_t hostrec_decide(int kind, bool have_base, bool have_owner, bool sealed_only);

#endif
