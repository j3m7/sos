// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// sos.hash, sos.hmac and sos.hashfile on core 0: see lua_hash.h.

#include "lua/lua_hash.h"

#include <string.h>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/hash.h>
#include <wolfssl/wolfcrypt/hmac.h>

#include "ff.h"
#include "pico/stdlib.h"

#include "core/heaptag.h"
#include "files/files.h"

typedef struct {
    bool hmac;
    enum wc_HashType type;
    union {
        wc_HashAlg hash;
        Hmac hmac;
    } u;
} ctx_t;

static ctx_t *ctx;                      // the hash running, if any (from the heap: none at rest)
static FIL *file;                       // sos.hashfile's file, while it's being read (heap)

static void close_file(void) {
    if (!file)
        return;
    f_close(file);
    heap_release(HEAP_LUA, file);
    file = NULL;
    files_release();
}

static const enum wc_HashType types[LUA_HASH_ALGS] = {
    WC_HASH_TYPE_SHA, WC_HASH_TYPE_SHA256, WC_HASH_TYPE_SHA384, WC_HASH_TYPE_SHA512,
};
static const int hmac_types[LUA_HASH_ALGS] = {WC_SHA, WC_SHA256, WC_SHA384, WC_SHA512};

void lua_hash_drop(void) {
    close_file();
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

// Open `path` for LUA_HASH_FILE. NULL, or why not.
static const char *open_file(const char *path) {
    file = heap_alloc(HEAP_LUA, sizeof(*file));
    if (!file)
        return "not enough memory";
    FRESULT fr = f_open(file, path, FA_READ);
    if (fr != FR_OK) {
        heap_release(HEAP_LUA, file);
        file = NULL;
        FILINFO fi;
        if (fr == FR_NO_FILE &&
            (!strcmp(path, "/") || (f_stat(path, &fi) == FR_OK && (fi.fattrib & AM_DIR))))
            return "is a directory";            // FatFs says FR_NO_FILE for one
        return files_error(fr);
    }
    files_hold();
    return NULL;
}

const char *lua_hash_file(const char *path, uint8_t *buf, size_t cap, uint64_t budget_us,
                          uint64_t *got) {
    *got = 0;
    if (!ctx || ctx->hmac)
        return "no hash running";
    const char *err = file ? NULL : open_file(path);
    uint64_t t0 = time_us_64();
    while (!err) {
        UINT n = 0;
        FRESULT fr = f_read(file, buf, (UINT)cap, &n);
        if (fr != FR_OK)
            err = files_error(fr);
        else if (n == 0 && *got == 0)
            close_file();                       // the end (said by a call that hashed nothing)
        else if (wc_HashUpdate(&ctx->u.hash, ctx->type, buf, n) != 0)
            err = "the hash failed";
        else
            *got += n;
        if (n == 0 || time_us_64() - t0 >= budget_us)
            break;
    }
    if (err)
        lua_hash_drop();
    return err;
}
