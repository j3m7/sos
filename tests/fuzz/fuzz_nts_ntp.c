// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// src/time/nts_ntp.c: an NTS-protected NTP reply from the network. The AEAD here accepts any
// tag and copies the ciphertext through, so the fuzzer also reaches the parsing of the
// encrypted extension fields (the new cookies), not just the unauthenticated part. The first
// 32 + 8 bytes are the request's unique identifier and transmit time the reply must echo.

#include <stdbool.h>
#include <string.h>

#include "fuzz.h"
#include "time/nts_ntp.h"

static bool any_seal(void *ctx, const uint8_t *ad, size_t adlen, const uint8_t *nonce,
                     size_t noncelen, const uint8_t *pt, size_t ptlen, uint8_t *out) {
    (void)ctx; (void)ad; (void)adlen; (void)nonce; (void)noncelen;
    memset(out, 0, 16);
    memmove(out + 16, pt, ptlen);
    return true;
}

static bool any_open(void *ctx, const uint8_t *ad, size_t adlen, const uint8_t *nonce,
                     size_t noncelen, const uint8_t *ct, size_t ctlen, uint8_t *pt) {
    (void)ctx; (void)ad; (void)adlen; (void)nonce; (void)noncelen;
    if (ctlen < 16)
        return false;
    memmove(pt, ct + 16, ctlen - 16);
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    uint8_t uid[NTS_NTP_UID_LEN] = {0}, xmt[8] = {0};
    for (size_t i = 0; i < sizeof(uid); i++)
        uid[i] = fuzz_byte(&data, &size);
    for (size_t i = 0; i < sizeof(xmt); i++)
        xmt[i] = fuzz_byte(&data, &size);
    if (size > NTS_NTP_PACKET_MAX)                  // nts.c receives into a buffer this size
        size = NTS_NTP_PACKET_MAX;
    static uint8_t pkt[NTS_NTP_PACKET_MAX], plain[NTS_NTP_PACKET_MAX];
    memcpy(pkt, data, size);
    const nts_aead_t aead = {any_seal, any_open, NULL};
    nts_ntp_reply_t reply;
    const char *why = NULL;
    nts_ntp_parse_reply(pkt, size, xmt, uid, &aead, plain, sizeof(plain), &reply, &why);
    return 0;
}
