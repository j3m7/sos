// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Does the key this image is signed with match a boot key in the board's OTP memory?
//
// With secure boot on, the boot ROM runs only an image signed with a key whose SHA-256 hash is
// in OTP (rows BOOTKEY0_0 onwards, four slots) and marked valid there. Turning secure boot on
// with the wrong hash in OTP leaves a board that runs nothing, for good. So before that bit is
// written, the firmware that is on the board says whether it would still start: the `bootkey`
// command, a step of docs/securing-a-board.md.
//
// This file is the part with no hardware in it, unit tested on the host: finding the public
// key in an image, the votes OTP flag rows are read with, and the comparison. It reads nothing
// and writes nothing. The shell reads the image from flash and the rows from OTP (core/otpread.h,
// which can only read) and hashes the key.

#ifndef SOS_BOOTKEY_H
#define SOS_BOOTKEY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOOTKEY_PUBKEY_LEN 64           // secp256k1 public key, x then y
#define BOOTKEY_HASH_LEN   32           // its SHA-256: what OTP holds
#define BOOTKEY_SLOTS      4
#define BOOTKEY_SLOT_ROWS  16           // a slot is 16 error-corrected rows of 16 bits

typedef enum {
    BOOTKEY_IMAGE_SIGNED,               // *pubkey points at the key, inside the image
    BOOTKEY_IMAGE_UNSIGNED,             // a well-formed image with no signature
    BOOTKEY_IMAGE_NO_BLOCKS,            // no closed loop of metadata blocks found: not an image
                                        // this code understands
} bootkey_image_t;

// Look for the image's metadata blocks (datasheet section 5.9: the first block starts within the
// image's first 4 kB, and the blocks link to each other in a closed loop), take the last image
// definition in the loop, which is the one the boot ROM uses, and return its signature's public
// key. `image` is the start of the image and must be word-aligned; nothing beyond `len` bytes is
// read.
bootkey_image_t bootkey_image_pubkey(const uint8_t *image, size_t len, const uint8_t **pubkey);

// OTP flag rows are stored several times over and read by vote: a bit counts as set when at
// least `need` of the `n` raw rows have it (3 of 8 for CRIT1, 2 of 3 for the boot flags).
uint32_t bootkey_vote(const uint32_t *rows, unsigned n, unsigned need);

// A slot's hash from its 16 rows: row k holds bytes 2k (low half) and 2k+1.
void bootkey_slot_hash(const uint16_t rows[BOOTKEY_SLOT_ROWS], uint8_t hash[BOOTKEY_HASH_LEN]);

// What OTP says about boot keys.
typedef struct {
    uint8_t slot[BOOTKEY_SLOTS][BOOTKEY_HASH_LEN];
    uint8_t valid;                      // BOOT_FLAGS1.KEY_VALID, bit n = slot n
    uint8_t invalid;                    // BOOT_FLAGS1.KEY_INVALID
} bootkey_otp_t;

// The boot ROM accepts a slot's key when it is marked valid and not marked invalid.
bool bootkey_slot_usable(const bootkey_otp_t *otp, unsigned slot);
bool bootkey_slot_blank(const bootkey_otp_t *otp, unsigned slot);   // all zero: never written

typedef enum {
    BOOTKEY_MATCH,                      // a usable slot holds this key's hash: with secure boot
                                        // on, this image would start
    BOOTKEY_MATCH_NOT_VALID,            // a slot holds it but isn't marked valid (yet)
    BOOTKEY_MATCH_INVALIDATED,          // a slot holds it and is marked invalid: revoked
    BOOTKEY_NO_MATCH,                   // no slot holds it
} bootkey_verdict_t;

// Compare the hash of the image's key with the slots. *slot is the slot the verdict is about
// (-1 for BOOTKEY_NO_MATCH). A usable match wins over any other.
bootkey_verdict_t bootkey_compare(const bootkey_otp_t *otp, const uint8_t hash[BOOTKEY_HASH_LEN],
                                  int *slot);

#endif
