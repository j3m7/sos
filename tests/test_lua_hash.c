// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/lua/lua_hash.c (sos.hash and sos.hmac on core 0), with wolfCrypt built for
// the host, against published test vectors: FIPS 180 ("abc", the million a's), RFC 2202
// (HMAC-SHA1) and RFC 4231 (HMAC-SHA2).

#include <stdio.h>
#include <string.h>

#include "core/heaptag.h"
#include "lua/lua_hash.h"
#include "test.h"

static char hex[2 * LUA_HASH_MAX + 1];

// Run a whole hash, the message in pieces of `piece` bytes (as core 1 sends it, 4 KB each); the
// digest in hex, or the error.
static const char *run(unsigned arg, const char *key, const char *msg, size_t len, size_t piece) {
    uint8_t out[LUA_HASH_MAX];
    size_t n = 0;
    const char *err = lua_hash_step(LUA_HASH_BEGIN, arg, (const uint8_t *)key, strlen(key), out,
                                    &n);
    for (size_t at = 0; !err && at < len; at += piece)
        err = lua_hash_step(LUA_HASH_UPDATE, arg, (const uint8_t *)msg + at,
                            len - at < piece ? len - at : piece, out, &n);
    if (!err)
        err = lua_hash_step(LUA_HASH_FINAL, arg, NULL, 0, out, &n);
    if (err)
        return err;
    for (size_t i = 0; i < n; i++)
        snprintf(hex + 2 * i, 3, "%02x", out[i]);
    return hex;
}

static const char *hash(unsigned alg, const char *msg) {
    return run(alg, "", msg, strlen(msg), 4096);
}

static const char *hmac(unsigned alg, const char *key, const char *msg) {
    return run(alg | LUA_HASH_HMAC, key, msg, strlen(msg), 4096);
}

#define CHECK_HEX(got, want) CHECK(!strcmp((got), (want)))

static void hashes(void) {
    CHECK_HEX(hash(LUA_HASH_SHA1, "abc"), "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK_HEX(hash(LUA_HASH_SHA256, "abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_HEX(hash(LUA_HASH_SHA256, ""),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK_HEX(hash(LUA_HASH_SHA384, "abc"),
              "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
              "8086072ba1e7cc2358baeca134c825a7");
    CHECK_HEX(hash(LUA_HASH_SHA512, "abc"),
              "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
              "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
}

static void million_as_in_any_pieces(void) {
    static char as[1000000];
    memset(as, 'a', sizeof(as));
    static const char *const want =
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
    CHECK_HEX(run(LUA_HASH_SHA256, "", as, sizeof(as), 4096), want);
    CHECK_HEX(run(LUA_HASH_SHA256, "", as, sizeof(as), 1), want);   // one byte at a time
    CHECK_HEX(run(LUA_HASH_SHA256, "", as, sizeof(as), 4093), want);
}

static void hmacs(void) {
    CHECK_HEX(hmac(LUA_HASH_SHA1, "Jefe", "what do ya want for nothing?"),
              "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
    CHECK_HEX(hmac(LUA_HASH_SHA256, "Jefe", "what do ya want for nothing?"),
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    CHECK_HEX(hmac(LUA_HASH_SHA384, "Jefe", "what do ya want for nothing?"),
              "af45d2e376484031617f78d2b58a6b1b9c7ef464f5a01b47e42ec3736322445e"
              "8e2240ca5e69e2c78b3239ecfab21649");
    CHECK_HEX(hmac(LUA_HASH_SHA512, "Jefe", "what do ya want for nothing?"),
              "164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea250554"
              "9758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737");
    // An empty key is defined (it's padded with zeros like any short one): the RFC 2104 value,
    // not an error.
    CHECK_HEX(hmac(LUA_HASH_SHA256, "", "x"),
              "4cbc96099a6467ce002461f10549b4898265ebe6188b45efacc44293516e62c4");
    CHECK_HEX(hmac(LUA_HASH_SHA1, "", ""), "fbdb1d1b18aa6c08324b7d64b71fb76370690e1d");
    // RFC 4231 test case 6: a key longer than the block (131 bytes of 0xaa), hashed first.
    char key[132];
    memset(key, 0xaa, 131);
    key[131] = '\0';
    CHECK_HEX(hmac(LUA_HASH_SHA256, key, "Test Using Larger Than Block-Size Key - Hash Key First"),
              "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

static void errors_and_memory(void) {
    uint8_t out[LUA_HASH_MAX];
    size_t n;
    CHECK_EQ(heap_tag_used(HEAP_LUA), 0);
    CHECK(!strcmp(lua_hash_step(LUA_HASH_UPDATE, LUA_HASH_SHA256, NULL, 0, out, &n),
                  "no hash running"));
    CHECK(!strcmp(lua_hash_step(LUA_HASH_FINAL, LUA_HASH_SHA256, NULL, 0, out, &n),
                  "no hash running"));
    CHECK(!strcmp(lua_hash_step(LUA_HASH_BEGIN, LUA_HASH_ALGS, NULL, 0, out, &n),
                  "unknown hash"));
    CHECK(!strcmp(lua_hash_step(9, LUA_HASH_SHA256, NULL, 0, out, &n), "unknown step"));
    CHECK_EQ(heap_tag_used(HEAP_LUA), 0);
    // An unfinished hash holds its context until dropped, or until the next one begins.
    CHECK(!lua_hash_step(LUA_HASH_BEGIN, LUA_HASH_SHA512 | LUA_HASH_HMAC, (const uint8_t *)"k", 1,
                         out, &n));
    CHECK(heap_tag_used(HEAP_LUA) > 0);
    CHECK_HEX(hash(LUA_HASH_SHA1, "abc"), "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK_EQ(heap_tag_used(HEAP_LUA), 0);
    CHECK(!lua_hash_step(LUA_HASH_BEGIN, LUA_HASH_SHA256, NULL, 0, out, &n));
    lua_hash_drop();
    CHECK_EQ(heap_tag_used(HEAP_LUA), 0);
    lua_hash_drop();                            // nothing to drop: no harm
}

TEST_MAIN(
    RUN(hashes);
    RUN(million_as_in_any_pieces);
    RUN(hmacs);
    RUN(errors_and_memory);
)
