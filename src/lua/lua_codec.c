// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Base64 and hex: see lua_codec.h.

#include "lua/lua_codec.h"

static const char std_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char url_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

size_t lua_b64_encoded_len(size_t n, bool url) {
    return url ? n / 3 * 4 + (n % 3 ? n % 3 + 1 : 0) : (n + 2) / 3 * 4;
}

void lua_b64_encode(const uint8_t *in, size_t n, char *out, bool url) {
    const char *a = url ? url_chars : std_chars;
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
        *out++ = a[v >> 18];
        *out++ = a[v >> 12 & 63];
        *out++ = a[v >> 6 & 63];
        *out++ = a[v & 63];
    }
    if (i < n) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0);
        *out++ = a[v >> 18];
        *out++ = a[v >> 12 & 63];
        if (i + 1 < n)
            *out++ = a[v >> 6 & 63];
        else if (!url)
            *out++ = '=';
        if (!url)
            *out++ = '=';
    }
}

// The 6-bit value of c in the alphabet, or -1.
static int value(char c, bool url) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == (url ? '-' : '+'))
        return 62;
    if (c == (url ? '_' : '/'))
        return 63;
    return -1;
}

bool lua_b64_decode(const char *in, size_t n, uint8_t *out, size_t *out_len, bool url) {
    uint32_t acc = 0;
    unsigned bits = 0, chars = 0, pad = 0;
    size_t len = 0;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        if (c == '=') {
            pad++;
            continue;
        }
        int v = value(c, url);
        if (v < 0 || pad)                       // only '=' (and spaces) after the first '='
            return false;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        chars++;
        if (bits >= 8) {
            bits -= 8;
            out[len++] = (uint8_t)(acc >> bits);
            acc &= (1u << bits) - 1;
        }
    }
    // A group of 4 can end in 2 or 3 characters (1 or 2 bytes), never 1; padding, if any, fills it.
    unsigned rest = chars % 4;
    if (rest == 1 || (pad && (rest == 0 || pad != 4 - rest)) || acc != 0)
        return false;
    *out_len = len;
    return true;
}

void lua_hex(const uint8_t *in, size_t n, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        *out++ = digits[in[i] >> 4];
        *out++ = digits[in[i] & 15];
    }
}
