// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Core 0's side of sos.hash and sos.hmac: wolfCrypt's hashes, run on core 0 because wolfCrypt
// allocates through the TLS module's accounting hooks (src/tls/tls.c), which only core 0 may
// use. One hash or HMAC at a time, streamed in LR_HASH requests (lua_bridge.h).

#ifndef SOS_LUA_HASH_H
#define SOS_LUA_HASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// LR_HASH's steps (lua_req.offset).
enum { LUA_HASH_BEGIN, LUA_HASH_UPDATE, LUA_HASH_FINAL };

// The algorithms (lua_req.arg's low byte), and LUA_HASH_HMAC or-ed in for an HMAC.
enum { LUA_HASH_SHA1, LUA_HASH_SHA256, LUA_HASH_SHA384, LUA_HASH_SHA512, LUA_HASH_ALGS };
#define LUA_HASH_HMAC 0x100u
#define LUA_HASH_MAX 64                 // the longest digest (SHA-512)

// One step. BEGIN: data is the HMAC key (ignored for a plain hash); a hash left unfinished is
// dropped. UPDATE: data is more of the message. FINAL: the digest into out (LUA_HASH_MAX bytes),
// its length in *out_len, and the context is freed. Returns NULL, or what went wrong (the context
// is then freed too).
const char *lua_hash_step(unsigned step, unsigned arg, const uint8_t *data, size_t len,
                          uint8_t *out, size_t *out_len);

// Free an unfinished hash (the script stopped, or ended, in the middle of one).
void lua_hash_drop(void);

#endif
