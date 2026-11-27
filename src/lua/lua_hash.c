// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// sos.hash and sos.hmac on core 0: see lua_hash.h.

#include "lua/lua_hash.h"

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/hash.h>
#include <wolfssl/wolfcrypt/hmac.h>

#include "core/heaptag.h"

typedef struct {
    bool hmac;
    enum wc_HashType type;
    union {
        wc_HashAlg hash;
        Hmac hmac;
    } u;
} ctx_t;

static ctx_t *ctx;                      // the hash running, if any (from the heap: none at rest)

static const enum wc_HashType types[LUA_HASH_ALGS] = {
    WC_HASH_TYPE_SHA, WC_HASH_TYPE_SHA256, WC_HASH_TYPE_SHA384, WC_HASH_TYPE_SHA512,
};
static const int hmac_types[LUA_HASH_ALGS] = {WC_SHA, WC_SHA256, WC_SHA384, WC_SHA512};

void lua_hash_drop(void) {
    if (!ctx)
        return;
    if (ctx->hmac)
        wc_HmacFree(&ctx->u.hmac);
    else
        wc_HashFree(&ctx->u.hash, ctx->type);
    heap_release(HEAP_LUA, ctx);
    ctx = NULL;
}

static const char *begin(unsigned arg, const uint8_t *key, size_t len) {
    unsigned alg = arg & 0xffu;
    if (alg >= LUA_HASH_ALGS)
        return "unknown hash";
    ctx = heap_alloc(HEAP_LUA, sizeof(*ctx));
    if (!ctx)
        return "not enough memory";
    ctx->hmac = (arg & LUA_HASH_HMAC) != 0;
    ctx->type = types[alg];
    int r;
    if (ctx->hmac) {
        r = wc_HmacInit(&ctx->u.hmac, NULL, INVALID_DEVID);
        if (r == 0)
            r = wc_HmacSetKey(&ctx->u.hmac, hmac_types[alg], key, (word32)len);
    } else {
        r = wc_HashInit(&ctx->u.hash, ctx->type);
    }
    return r == 0 ? NULL : "the hash couldn't start";
}

const char *lua_hash_step(unsigned step, unsigned arg, const uint8_t *data, size_t len,
                          uint8_t *out, size_t *out_len) {
    const char *err = NULL;
    int r = 0;
    switch (step) {
    case LUA_HASH_BEGIN:
        lua_hash_drop();
        err = begin(arg, data, len);
        break;
    case LUA_HASH_UPDATE:
        if (!ctx)
            return "no hash running";
        r = ctx->hmac ? wc_HmacUpdate(&ctx->u.hmac, data, (word32)len)
                      : wc_HashUpdate(&ctx->u.hash, ctx->type, data, (word32)len);
        break;
    case LUA_HASH_FINAL:
        if (!ctx)
            return "no hash running";
        r = ctx->hmac ? wc_HmacFinal(&ctx->u.hmac, out)
                      : wc_HashFinal(&ctx->u.hash, ctx->type, out);
        *out_len = (size_t)wc_HashGetDigestSize(ctx->type);
        lua_hash_drop();
        break;
    default:
        err = "unknown step";
        break;
    }
    if (r != 0)
        err = "the hash failed";
    if (err)
        lua_hash_drop();
    return err;
}
