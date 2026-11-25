// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// SD card encryption: header and sectors (see sdcrypt.h). Pure C over wolfCrypt.

#include "files/sdcrypt.h"

#include <string.h>

static const uint8_t magic[16] = "SOS-CRYPT-CARD\0";    // 14 characters and two NULs

enum {
    OFF_MAGIC = 0, OFF_VERSION = 16, OFF_CIPHER = 17, OFF_WRAP = 18, OFF_OFFSET = 20,
    OFF_CARD_ID = 24, OFF_NONCE = 40, OFF_KEY = 52, OFF_TAG = 84, HEADER_END = 100,
    AAD_LEN = OFF_NONCE,
};
_Static_assert(OFF_TAG == OFF_KEY + SDCRYPT_KEY_LEN, "layout");

#define VERSION     1
#define CIPHER_XTS_AES_128  1
#define WRAP_GCM_DEVKEY     1       // the old device key: recognised, not opened
#define WRAP_GCM_CARDKEY    2
#define TAG_LEN     16

bool sdcrypt_wrap_key(uint8_t out[SDCRYPT_DEVKEY_LEN], const uint8_t base[SDCRYPT_DEVKEY_LEN],
                      const uint8_t owner[32]) {
    Hmac hmac;
    bool init = wc_HmacInit(&hmac, NULL, INVALID_DEVID) == 0;
    bool ok = init && wc_HmacSetKey(&hmac, WC_SHA256, base, SDCRYPT_DEVKEY_LEN) == 0 &&
              wc_HmacUpdate(&hmac, owner, 32) == 0 && wc_HmacFinal(&hmac, out) == 0;
    if (init)
        wc_HmacFree(&hmac);
    sdcrypt_wipe(&hmac, sizeof(hmac));
    if (!ok)
        sdcrypt_wipe(out, SDCRYPT_DEVKEY_LEN);
    return ok;
}

void sdcrypt_wipe(void *p, unsigned len) {
    volatile uint8_t *v = p;
    while (len--)
        *v++ = 0;
}

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool sdcrypt_header_build(uint8_t sector[SDCRYPT_SECTOR], const uint8_t devkey[SDCRYPT_DEVKEY_LEN],
                          const uint8_t card_id[SDCRYPT_ID_LEN],
                          const uint8_t nonce[SDCRYPT_NONCE_LEN],
                          const uint8_t data_key[SDCRYPT_KEY_LEN]) {
    memset(sector, 0, SDCRYPT_SECTOR);
    memcpy(sector + OFF_MAGIC, magic, sizeof(magic));
    sector[OFF_VERSION] = VERSION;
    sector[OFF_CIPHER] = CIPHER_XTS_AES_128;
    sector[OFF_WRAP] = WRAP_GCM_CARDKEY;
    put_le32(sector + OFF_OFFSET, SDCRYPT_OFFSET);
    memcpy(sector + OFF_CARD_ID, card_id, SDCRYPT_ID_LEN);
    memcpy(sector + OFF_NONCE, nonce, SDCRYPT_NONCE_LEN);

    Aes gcm;
    bool ok = wc_AesInit(&gcm, NULL, INVALID_DEVID) == 0 &&
              wc_AesGcmSetKey(&gcm, devkey, SDCRYPT_DEVKEY_LEN) == 0 &&
              wc_AesGcmEncrypt(&gcm, sector + OFF_KEY, data_key, SDCRYPT_KEY_LEN,
                               sector + OFF_NONCE, SDCRYPT_NONCE_LEN, sector + OFF_TAG, TAG_LEN,
                               sector, AAD_LEN) == 0;
    wc_AesFree(&gcm);
    sdcrypt_wipe(&gcm, sizeof(gcm));
    if (!ok)
        memset(sector, 0, SDCRYPT_SECTOR);
    return ok;
}

bool sdcrypt_is_header(const uint8_t sector[SDCRYPT_SECTOR]) {
    return !memcmp(sector + OFF_MAGIC, magic, sizeof(magic));
}

int sdcrypt_header_open(const uint8_t sector[SDCRYPT_SECTOR],
                        const uint8_t devkey[SDCRYPT_DEVKEY_LEN], sdcrypt_info_t *info) {
    memset(info, 0, sizeof(*info));
    if (!sdcrypt_is_header(sector))
        return SDCRYPT_PLAIN;
    if (sector[OFF_VERSION] != VERSION || sector[OFF_CIPHER] != CIPHER_XTS_AES_128 ||
        get_le32(sector + OFF_OFFSET) != SDCRYPT_OFFSET)
        return SDCRYPT_UNSUPPORTED;
    if (sector[OFF_WRAP] == WRAP_GCM_DEVKEY)
        return SDCRYPT_OLD_WRAP;
    if (sector[OFF_WRAP] != WRAP_GCM_CARDKEY)
        return SDCRYPT_UNSUPPORTED;

    Aes gcm;
    bool ok = wc_AesInit(&gcm, NULL, INVALID_DEVID) == 0 &&
              wc_AesGcmSetKey(&gcm, devkey, SDCRYPT_DEVKEY_LEN) == 0 &&
              wc_AesGcmDecrypt(&gcm, info->data_key, sector + OFF_KEY, SDCRYPT_KEY_LEN,
                               sector + OFF_NONCE, SDCRYPT_NONCE_LEN, sector + OFF_TAG, TAG_LEN,
                               sector, AAD_LEN) == 0;
    wc_AesFree(&gcm);
    sdcrypt_wipe(&gcm, sizeof(gcm));
    if (!ok) {
        sdcrypt_wipe(info, sizeof(*info));
        return SDCRYPT_WRONG_KEY;
    }
    memcpy(info->card_id, sector + OFF_CARD_ID, SDCRYPT_ID_LEN);
    info->offset = SDCRYPT_OFFSET;
    return SDCRYPT_OK;
}

static int open_one(const uint8_t *sector, const uint8_t *devkey, sdcrypt_info_t *info) {
    if (!sdcrypt_is_header(sector)) {
        memset(info, 0, sizeof(*info));
        return SDCRYPT_PLAIN;
    }
    if (!devkey) {
        memset(info, 0, sizeof(*info));
        return sector[OFF_WRAP] == WRAP_GCM_DEVKEY ? SDCRYPT_OLD_WRAP : SDCRYPT_NO_DEVKEY;
    }
    return sdcrypt_header_open(sector, devkey, info);
}

int sdcrypt_card_open(const uint8_t primary[SDCRYPT_SECTOR], const uint8_t *backup,
                      const uint8_t *devkey, sdcrypt_info_t *info, bool *from_backup) {
    *from_backup = false;
    int r = open_one(primary, devkey, info);
    if (r == SDCRYPT_OK)
        return r;
    bool boot_sector = r == SDCRYPT_PLAIN && primary[510] == 0x55 && primary[511] == 0xaa;
    if (boot_sector || !backup || !sdcrypt_is_header(backup))
        return r;
    int rb = open_one(backup, devkey, info);
    if (rb == SDCRYPT_OK) {
        *from_backup = true;
        return rb;
    }
    return r == SDCRYPT_PLAIN ? rb : r;
}

bool sdcrypt_start(sdcrypt_t *c, const uint8_t data_key[SDCRYPT_KEY_LEN]) {
    sdcrypt_stop(c);
    if (wc_AesXtsSetKey(&c->enc, data_key, SDCRYPT_KEY_LEN, AES_ENCRYPTION, NULL, INVALID_DEVID) ||
        wc_AesXtsSetKey(&c->dec, data_key, SDCRYPT_KEY_LEN, AES_DECRYPTION, NULL, INVALID_DEVID)) {
        sdcrypt_stop(c);
        return false;
    }
    c->ready = true;
    return true;
}

void sdcrypt_stop(sdcrypt_t *c) {
    if (c->ready) {
        wc_AesXtsFree(&c->enc);
        wc_AesXtsFree(&c->dec);
    }
    sdcrypt_wipe(c, sizeof(*c));
}

bool sdcrypt_encrypt(sdcrypt_t *c, uint8_t *out, const uint8_t *in, uint32_t lsector, uint32_t count) {
    if (!c->ready)
        return false;
    for (uint32_t i = 0; i < count; i++)
        if (wc_AesXtsEncryptSector(&c->enc, out + i * SDCRYPT_SECTOR, in + i * SDCRYPT_SECTOR,
                                   SDCRYPT_SECTOR, (word64)lsector + i))
            return false;
    return true;
}

bool sdcrypt_decrypt(sdcrypt_t *c, uint8_t *buf, uint32_t lsector, uint32_t count) {
    if (!c->ready)
        return false;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *s = buf + i * SDCRYPT_SECTOR;
        if (wc_AesXtsDecryptSector(&c->dec, s, s, SDCRYPT_SECTOR, (word64)lsector + i))
            return false;
    }
    return true;
}
