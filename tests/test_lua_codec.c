// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/lua/lua_codec.c: base64 (RFC 4648 section 10's vectors, the URL-safe
// alphabet, round trips of every byte value, what decoding refuses) and hex.

#include <stdlib.h>
#include <string.h>

#include "lua/lua_codec.h"
#include "test.h"

static char enc[512];
static uint8_t dec[512];

static const char *encode(const char *s, size_t n, bool url) {
    size_t len = lua_b64_encoded_len(n, url);
    lua_b64_encode((const uint8_t *)s, n, enc, url);
    enc[len] = '\0';
    return enc;
}

static bool decodes_to(const char *in, const char *want, bool url) {
    size_t n = 0;
    if (!lua_b64_decode(in, strlen(in), dec, &n, url))
        return false;
    return n == strlen(want) && !memcmp(dec, want, n);
}

static bool refused(const char *in, bool url) {
    size_t n;
    return !lua_b64_decode(in, strlen(in), dec, &n, url);
}

static void rfc4648_vectors(void) {
    static const char *const plain[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    static const char *const std[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=",
                                      "Zm9vYmFy"};
    static const char *const url[] = {"", "Zg", "Zm8", "Zm9v", "Zm9vYg", "Zm9vYmE", "Zm9vYmFy"};
    for (int i = 0; i < 7; i++) {
        size_t n = strlen(plain[i]);
        CHECK(!strcmp(encode(plain[i], n, false), std[i]));
        CHECK(!strcmp(encode(plain[i], n, true), url[i]));
        CHECK(decodes_to(std[i], plain[i], false));
        CHECK(decodes_to(url[i], plain[i], true));
        CHECK(decodes_to(url[i], plain[i], false));    // padding is optional
        CHECK(decodes_to(std[i], plain[i], true));
    }
}

static void url_alphabet(void) {
    static const char bytes[] = "\xfb\xff\xbf";        // 111110 111111 111110 111111
    CHECK(!strcmp(encode(bytes, 3, false), "+/+/"));
    CHECK(!strcmp(encode(bytes, 3, true), "-_-_"));
    CHECK(decodes_to("+/+/", bytes, false));
    CHECK(decodes_to("-_-_", bytes, true));
    CHECK(refused("-_-_", false));
    CHECK(refused("+/+/", true));
}

static void round_trips(void) {
    char in[300];
    srand(208);
    for (int len = 0; len <= 260; len++) {
        for (int i = 0; i < len; i++)
            in[i] = (char)(len < 256 ? i : rand());   // every byte value, then random
        for (int url = 0; url <= 1; url++) {
            encode(in, (size_t)len, url);
            size_t n = 0;
            CHECK(lua_b64_decode(enc, strlen(enc), dec, &n, url));
            CHECK(n == (size_t)len && !memcmp(dec, in, n));
        }
    }
}

static void decoding_rules(void) {
    CHECK(decodes_to("Zm9v\r\nYmFy", "foobar", false));        // line breaks and spaces skipped
    CHECK(decodes_to(" Zm9v YmE= ", "fooba", false));
    CHECK(refused("Z", false));                                // a lone character over
    CHECK(refused("Zm9vY", false));
    CHECK(refused("Zg=", false));                              // padding, but not all of it
    CHECK(refused("Zm9v=", false));                            // padding with nothing to fill
    CHECK(refused("Zg==Zg==", false));                         // more after the padding
    CHECK(refused("Zh==", false));                             // bits left over that aren't 0
    CHECK(refused("Zm9v!", false));
    CHECK(refused("Zm9\x80", false));
}

static void hex(void) {
    char out[8];
    lua_hex((const uint8_t *)"\x00\x0f\xa5\xff", 4, out);
    CHECK(!memcmp(out, "000fa5ff", 8));
}

TEST_MAIN(
    RUN(rfc4648_vectors);
    RUN(url_alphabet);
    RUN(round_trips);
    RUN(decoding_rules);
    RUN(hex);
)
