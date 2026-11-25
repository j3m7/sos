// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// SD card encryption: the card header and sector encryption. Pure C over the wolfCrypt API, so
// it's host tested with wolfCrypt built for the host; sdcard.c does the I/O.
//
//   physical sector 0      header (plain)
//   physical sector 4096   backup copy of the header
//   physical 8192 ...      the encrypted volume: logical sector n is physical n + 8192
//
// Each logical sector is encrypted with XTS-AES-128 under the card's data key, with the logical
// sector number as the tweak, little-endian (dm-crypt's "plain64"), so Linux can open a card
// with `cryptsetup open --type plain --cipher aes-xts-plain64 --key-size 256 --offset 8192`.
// The data key is random per card and stored in the header encrypted with AES-256-GCM under
// the device key; the GCM associated data is the header fields before the nonce.
//
// Header layout (512 bytes, multi-byte numbers little-endian):
//    0  magic "SOS-CRYPT-CARD\0\0"     16
//   16  version (1)                    1
//   17  cipher (1 = XTS-AES-128)       1
//   18  wrap (1 = AES-256-GCM, device key)  1
//   19  reserved (0)                   1
//   20  data offset in sectors (8192)  4
//   24  card id (random)               16
//   40  nonce                          12
//   52  data key, encrypted            32
//   84  GCM tag                        16
//  100  zero to the end of the sector

#ifndef SOS_SDCRYPT_H
#define SOS_SDCRYPT_H

#include <stdbool.h>
#include <stdint.h>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/aes.h>

#define SDCRYPT_SECTOR      512
#define SDCRYPT_OFFSET      8192u       // first sector of the encrypted volume (4 MB)
#define SDCRYPT_BACKUP      4096u       // sector of the header's backup copy
#define SDCRYPT_KEY_LEN     32          // data key: XTS-AES-128 (two 128-bit keys)
#define SDCRYPT_DEVKEY_LEN  32          // device key: AES-256-GCM
#define SDCRYPT_ID_LEN      16
#define SDCRYPT_NONCE_LEN   12

enum {
    SDCRYPT_OK = 0,             // a header, and the device key opened it
    SDCRYPT_PLAIN,              // not a header: a plain card (or not this sector)
    SDCRYPT_UNSUPPORTED,        // a header of a version or cipher this firmware doesn't know
    SDCRYPT_WRONG_KEY,          // a header the device key doesn't open (another device, or damaged)
};

typedef struct {
    uint8_t card_id[SDCRYPT_ID_LEN];
    uint8_t data_key[SDCRYPT_KEY_LEN];
    uint32_t offset;
} sdcrypt_info_t;

// Build a header sector for a new card. The caller supplies the random card id, nonce and data
// key. False if encryption failed.
bool sdcrypt_header_build(uint8_t sector[SDCRYPT_SECTOR], const uint8_t devkey[SDCRYPT_DEVKEY_LEN],
                          const uint8_t card_id[SDCRYPT_ID_LEN],
                          const uint8_t nonce[SDCRYPT_NONCE_LEN],
                          const uint8_t data_key[SDCRYPT_KEY_LEN]);

// Is this sector a header (magic only, no key needed)?
bool sdcrypt_is_header(const uint8_t sector[SDCRYPT_SECTOR]);

// Read a header sector and unwrap its data key into `info` (SDCRYPT_OK only; wiped otherwise).
int sdcrypt_header_open(const uint8_t sector[SDCRYPT_SECTOR],
                        const uint8_t devkey[SDCRYPT_DEVKEY_LEN], sdcrypt_info_t *info);

// Sector encryption with a card's data key.
typedef struct {
    XtsAes enc, dec;
    bool ready;
} sdcrypt_t;

bool sdcrypt_start(sdcrypt_t *c, const uint8_t data_key[SDCRYPT_KEY_LEN]);
void sdcrypt_stop(sdcrypt_t *c);                // forgets the key
// `count` sectors from logical sector `lsector`. Encrypt from `in` to `out` (separate buffers);
// decrypt in place.
bool sdcrypt_encrypt(sdcrypt_t *c, uint8_t *out, const uint8_t *in, uint32_t lsector, uint32_t count);
bool sdcrypt_decrypt(sdcrypt_t *c, uint8_t *buf, uint32_t lsector, uint32_t count);

// Clear memory that held a key (not optimized away).
void sdcrypt_wipe(void *p, unsigned len);

#endif
