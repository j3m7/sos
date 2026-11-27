// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Base64 (RFC 4648: the standard alphabet with padding, or the URL-safe one without) and hex,
// for sos.base64 and the hex digests of sos.hash and sos.hmac. Pure C, on core 1, in the host
// tests.

#ifndef SOS_LUA_CODEC_H
#define SOS_LUA_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The encoded length of n bytes (no NUL).
size_t lua_b64_encoded_len(size_t n, bool url);

// Encode n bytes into out (lua_b64_encoded_len() bytes; no NUL written). url: '-' and '_' for
// '+' and '/', and no '=' padding (as JWTs and URLs use it).
void lua_b64_encode(const uint8_t *in, size_t n, char *out, bool url);

// Decode n characters into out (at least n * 3 / 4 bytes), its length in *out_len. Spaces, tabs
// and line breaks are skipped; padding is optional, but only at the end. url: the URL-safe
// alphabet instead of the standard one. False for anything else (a character outside the
// alphabet, misplaced '=', a length that leaves 1 character over).
bool lua_b64_decode(const char *in, size_t n, uint8_t *out, size_t *out_len, bool url);

// Lower-case hex of n bytes into out (2 * n characters; no NUL written).
void lua_hex(const uint8_t *in, size_t n, char *out);

#endif
