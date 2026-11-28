// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Core 0's side of sos.hash and sos.hmac: wolfCrypt's hashes, run on core 0 because wolfCrypt
// allocates through the TLS module's accounting hooks (src/tls/tls.c), which only core 0 may
// use. One hash or HMAC at a time, streamed in LR_HASH requests (lua_bridge.h). sos.hashfile's
// file is read here too (LUA_HASH_FILE), so its bytes never go to core 1.

#ifndef SOS_LUA_HASH_H
#define SOS_LUA_HASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// LR_HASH's steps (lua_req.offset).
enum { LUA_HASH_BEGIN, LUA_HASH_UPDATE, LUA_HASH_FINAL, LUA_HASH_FILE };

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

// LUA_HASH_FILE: more of the file at `path` (absolute, normalized) into the running hash, read
// in pieces of `cap` bytes through buf until `budget_us` has passed or the file ends. The first
// call opens the file and holds it (files_hold()) until it ends, the hash is dropped, or a new
// one begins; later calls carry on where the last one stopped and ignore `path`. *got is the
// bytes hashed by this call, 0 once the file has ended (it's then closed; take the digest with
// LUA_HASH_FINAL). Returns NULL, or what went wrong (the hash and the file are then dropped).
const char *lua_hash_file(const char *path, uint8_t *buf, size_t cap, uint64_t budget_us,
                          uint64_t *got);

// Free an unfinished hash, and close its file (the script stopped, or ended, in the middle of
// one).
void lua_hash_drop(void);

#endif
