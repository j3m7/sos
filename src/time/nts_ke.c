// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "time/nts_ke.h"

#include <string.h>

#define CRITICAL 0x8000u

static size_t put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
    return 2;
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

size_t nts_ke_build_request(uint8_t *buf, size_t buflen) {
    if (buflen < 16)
        return 0;
    uint8_t *p = buf;
    // NTS Next Protocol Negotiation: must be critical.
    p += put_u16(p, CRITICAL | NTS_KE_NEXT_PROTOCOL);
    p += put_u16(p, 2);
    p += put_u16(p, NTS_PROTOCOL_NTPV4);
    // AEAD Algorithm Negotiation: the critical bit is optional; left clear.
    p += put_u16(p, NTS_KE_AEAD);
    p += put_u16(p, 2);
    p += put_u16(p, NTS_AEAD_AES_SIV_CMAC_256);
    // End of Message: must be critical.
    p += put_u16(p, CRITICAL | NTS_KE_END);
    p += put_u16(p, 0);
    return (size_t)(p - buf);
}

nts_ke_status_t nts_ke_parse(const uint8_t *buf, size_t len, nts_ke_result_t *res,
                             const char **reason) {
    memset(res, 0, sizeof(*res));
    res->error_code = res->warning_code = -1;
    bool have_protocol = false, have_aead = false;
    const char *why = NULL;
    size_t off = 0;

    while (!why) {
        if (len - off < 4)
            return NTS_KE_NEED_MORE;
        uint16_t head = get_u16(buf + off);
        uint16_t blen = get_u16(buf + off + 2);
        bool critical = head & CRITICAL;
        unsigned type = head & ~CRITICAL;
        if (len - off - 4 < blen) {
            if (off + 4 + (size_t)blen > NTS_KE_RESPONSE_MAX)
                why = "response too long";
            else
                return NTS_KE_NEED_MORE;
            break;
        }
        const uint8_t *body = buf + off + 4;
        off += 4 + (size_t)blen;

        switch (type) {
        case NTS_KE_END:
            if (blen != 0)
                why = "End of Message with a body";
            else if (off != len)
                why = "data after End of Message";
            else if (res->error_code >= 0)
                why = "server sent an Error record";
            else if (res->warning_code >= 0)
                why = "server sent a Warning record";
            else if (!have_protocol)
                why = "no Next Protocol record";
            else if (!have_aead)
                why = "no AEAD record";
            else if (res->cookie_count == 0)
                why = "no cookies";
            else {
                *reason = NULL;
                return NTS_KE_OK;
            }
            break;
        case NTS_KE_NEXT_PROTOCOL:
            // The server answers with the one protocol it chose; an empty list means none.
            if (have_protocol)
                why = "two Next Protocol records";
            else if (blen != 2 || get_u16(body) != NTS_PROTOCOL_NTPV4)
                why = "server didn't choose NTPv4";
            have_protocol = true;
            break;
        case NTS_KE_ERROR:
        case NTS_KE_WARNING:
            if (blen != 2)
                why = "malformed Error/Warning record";
            else if (type == NTS_KE_ERROR)
                res->error_code = get_u16(body);
            else
                res->warning_code = get_u16(body);
            break;
        case NTS_KE_AEAD:
            if (have_aead)
                why = "two AEAD records";
            else if (blen != 2 || get_u16(body) != NTS_AEAD_AES_SIV_CMAC_256)
                why = "server didn't choose AES-SIV-CMAC-256";
            res->aead = blen == 2 ? get_u16(body) : 0;
            have_aead = true;
            break;
        case NTS_KE_COOKIE:
            if (blen == 0 || blen > NTS_KE_COOKIE_MAX)
                why = "cookie of unsupported size";
            else if (res->cookie_count < NTS_KE_MAX_COOKIES) {   // extras are ignored
                memcpy(res->cookies[res->cookie_count], body, blen);
                res->cookie_len[res->cookie_count++] = blen;
            }
            break;
        case NTS_KE_NTPV4_SERVER:
            if (blen == 0 || blen > NTS_KE_SERVER_MAX || memchr(body, '\0', blen))
                why = "malformed server record";
            else {
                memcpy(res->server, body, blen);
                res->server[blen] = '\0';
            }
            break;
        case NTS_KE_NTPV4_PORT:
            if (blen != 2 || get_u16(body) == 0)
                why = "malformed port record";
            else
                res->port = get_u16(body);
            break;
        default:
            if (critical)
                why = "unknown critical record";
            break;                              // unknown non-critical records are ignored
        }
    }
    *reason = why;
    return NTS_KE_INVALID;
}

void nts_ke_exporter_context(uint16_t aead, bool s2c, uint8_t ctx[5]) {
    put_u16(ctx, NTS_PROTOCOL_NTPV4);
    put_u16(ctx + 2, aead);
    ctx[4] = s2c ? 1 : 0;
}
