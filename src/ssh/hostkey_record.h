// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The SSH host key's record in the key sector (one 256-byte flash page). Pure C, host tested;
// ssh/hostkey.c does the flash and the key generation.
//
// Sealed record, written by this firmware (multi-byte numbers little-endian):
//    0  magic "SOSK"        4   (0x4b534f53)
//    4  version (2)         4
//    8  key length n        4   (the private key, DER; at most HOSTREC_MAX_KEY)
//   12  the key, sealed     n + 28: nonce (12), ciphertext (n), tag (16)   (core/sealer.h)
//       erased (0xff) to the end of the page
// The associated data of the seal is the first 12 bytes, so the sealed key can't be put under
// another length or version.
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
    HOSTREC_SEALED,         // a sealed record (whether it opens is hostrec_open's answer)
};

// What the page holds, from its header alone (and the check, for a plain record).
int hostrec_kind(const uint8_t page[HOSTREC_PAGE]);

// Copy the key out of the page into der (HOSTREC_PAGE bytes of room) and set *len. A sealed
// record is opened with `sealer` (NULL: it can't be). False, with der zeroed, if the page holds
// no usable record or the sealed one doesn't open.
bool hostrec_open(const uint8_t page[HOSTREC_PAGE], const sealer_t *sealer, uint8_t *der,
                  uint32_t *len);

// Build a sealed record for a key in `page`. False (page zeroed) if the key is too long or
// sealing failed.
bool hostrec_seal(uint8_t page[HOSTREC_PAGE], const sealer_t *sealer, const uint8_t *der,
                  uint32_t len);

#endif
