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
// the board's card key (files/cardkey.h: derived from the root secret and the configuration's
// owner secret); the GCM associated data is the header fields before the nonce.
//
// Header layout (512 bytes, multi-byte numbers little-endian):
//    0  magic "SOS-CRYPT-CARD\0\0"     16
//   16  version (1)                    1
//   17  cipher (1 = XTS-AES-128)       1
//   18  wrap (2 = AES-256-GCM, the card key; 1 = the same under the device key that firmware
//       before the owner secret kept in flash: no longer opened)  1
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
#include <wolfssl/wolfcrypt/hmac.h>

#define SDCRYPT_SECTOR      512
#define SDCRYPT_OFFSET      8192u       // first sector of the encrypted volume (4 MB)
#define SDCRYPT_BACKUP      4096u       // sector of the header's backup copy
#define SDCRYPT_KEY_LEN     32          // data key: XTS-AES-128 (two 128-bit keys)
#define SDCRYPT_DEVKEY_LEN  32          // the board's card key: AES-256-GCM
#define SDCRYPT_ID_LEN      16
#define SDCRYPT_NONCE_LEN   12

enum {
    SDCRYPT_OK = 0,             // a header, and the card key opened it
    SDCRYPT_PLAIN,              // not a header: a plain card (or not this sector)
    SDCRYPT_UNSUPPORTED,        // a header of a version or cipher this firmware doesn't know
    SDCRYPT_WRONG_KEY,          // a header the card key doesn't open (another board or owner, or damaged)
    SDCRYPT_NO_DEVKEY,          // a header, but there's no card key to open it with
    SDCRYPT_OLD_WRAP,           // a header wrapped under the old device key: not opened any more
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

// Open a card from its sector 0 (`primary`) and the backup copy at SDCRYPT_BACKUP (`backup`,
// NULL if it couldn't be read), with the board's card key (`devkey`; NULL if there is none):
// - sector 0 is a header that opens: SDCRYPT_OK.
// - otherwise the backup, if it's a header, unless sector 0 is a plain card's boot sector or
//   MBR (ends in 0x55AA: a plain card, or one a PC formatted over an encrypted one). If the
//   backup opens, SDCRYPT_OK with *from_backup set (sector 0 should then be rewritten from it).
// - no header anywhere: SDCRYPT_PLAIN; else why it didn't open (sector 0's reason first).
int sdcrypt_card_open(const uint8_t primary[SDCRYPT_SECTOR], const uint8_t *backup,
                      const uint8_t *devkey, sdcrypt_info_t *info, bool *from_backup);

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

// The card key: HMAC-SHA256(key = base, message = owner). False (out zeroed) on failure.
bool sdcrypt_wrap_key(uint8_t out[SDCRYPT_DEVKEY_LEN], const uint8_t base[SDCRYPT_DEVKEY_LEN],
                      const uint8_t owner[32]);

// Clear memory that held a key (not optimized away).
void sdcrypt_wipe(void *p, unsigned len);

#endif
