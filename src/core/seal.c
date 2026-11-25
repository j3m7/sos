// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// AES-256-GCM sealing of stored records (see seal.h). Pure C over wolfCrypt.

#include "core/seal.h"

#include <string.h>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/aes.h>

static void wipe(void *p, size_t len) {
    volatile uint8_t *v = p;
    while (len--)
        *v++ = 0;
}

void seal_wipe(seal_ctx_t *ctx) {
    wipe(ctx->key, sizeof(ctx->key));
    ctx->have_key = false;
}

bool seal_seal(void *vctx, const uint8_t *aad, size_t aad_len, const uint8_t *plain, size_t len,
               uint8_t *out) {
    const seal_ctx_t *ctx = vctx;
    uint8_t *nonce = out, *cipher = out + SEALER_NONCE_LEN, *tag = cipher + len;
    Aes gcm;
    bool ok = ctx->have_key && ctx->random && len > 0 &&
              ctx->random(ctx->random_ctx, nonce, SEALER_NONCE_LEN);
    bool init = ok && wc_AesInit(&gcm, NULL, INVALID_DEVID) == 0;
    ok = init && wc_AesGcmSetKey(&gcm, ctx->key, SEAL_KEY_LEN) == 0 &&
         wc_AesGcmEncrypt(&gcm, cipher, plain, (word32)len, nonce, SEALER_NONCE_LEN, tag,
                          SEALER_TAG_LEN, aad, (word32)aad_len) == 0;
    if (init) {
        wc_AesFree(&gcm);
        wipe(&gcm, sizeof(gcm));
    }
    if (!ok)
        memset(out, 0, len + SEALER_OVERHEAD);
    return ok;
}

bool seal_open(void *vctx, const uint8_t *aad, size_t aad_len, const uint8_t *sealed,
               size_t sealed_len, uint8_t *plain) {
    const seal_ctx_t *ctx = vctx;
    if (sealed_len <= SEALER_OVERHEAD)
        return false;
    size_t len = sealed_len - SEALER_OVERHEAD;
    const uint8_t *nonce = sealed, *cipher = sealed + SEALER_NONCE_LEN, *tag = cipher + len;
    Aes gcm;
    bool init = ctx->have_key && wc_AesInit(&gcm, NULL, INVALID_DEVID) == 0;
    bool ok = init && wc_AesGcmSetKey(&gcm, ctx->key, SEAL_KEY_LEN) == 0 &&
              wc_AesGcmDecrypt(&gcm, plain, cipher, (word32)len, nonce, SEALER_NONCE_LEN, tag,
                               SEALER_TAG_LEN, aad, (word32)aad_len) == 0;
    if (init) {
        wc_AesFree(&gcm);
        wipe(&gcm, sizeof(gcm));
    }
    if (!ok)
        wipe(plain, len);       // wolfCrypt may have written unauthenticated plaintext
    return ok;
}
