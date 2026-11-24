// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "nts_ntp.h"

#include <string.h>

#define NTP_UNIX_DIFF 2208988800LL      // seconds from 1900-01-01 to 1970-01-01

static size_t pad4(size_t n) {
    return (n + 3) & ~(size_t)3;
}

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t get_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t get_u64(const uint8_t *p) {
    return ((uint64_t)get_u32(p) << 32) | get_u32(p + 4);
}

// Append an extension field: header, body, zero padding to a multiple of 4.
static uint8_t *put_ef(uint8_t *p, uint16_t type, const uint8_t *body, size_t len) {
    size_t total = 4 + pad4(len);
    put_u16(p, type);
    put_u16(p + 2, (uint16_t)total);
    if (body)
        memcpy(p + 4, body, len);
    else
        memset(p + 4, 0, len);
    memset(p + 4 + len, 0, pad4(len) - len);
    return p + total;
}

size_t nts_ntp_build_request(uint8_t *buf, size_t buflen, const uint8_t xmt[8],
                             const uint8_t uid[NTS_NTP_UID_LEN], const uint8_t *cookie,
                             size_t cookie_len, unsigned placeholders,
                             const uint8_t nonce[NTS_NTP_NONCE_LEN], const nts_aead_t *c2s) {
    if (cookie_len == 0 || cookie_len > NTS_NTP_COOKIE_MAX || placeholders >= NTS_NTP_MAX_COOKIES)
        return 0;
    size_t cookie_ef = 4 + pad4(cookie_len);
    size_t auth_ef = 4 + 4 + NTS_NTP_NONCE_LEN + NTS_NTP_TAG_LEN;
    size_t need = NTS_NTP_HEADER_LEN + 4 + NTS_NTP_UID_LEN + cookie_ef * (1 + placeholders) +
                  auth_ef;
    if (need > buflen)
        return 0;

    memset(buf, 0, NTS_NTP_HEADER_LEN);
    buf[0] = (0 << 6) | (4 << 3) | 3;           // no leap warning, NTPv4, client
    memcpy(buf + 40, xmt, 8);                   // transmit timestamp
    uint8_t *p = buf + NTS_NTP_HEADER_LEN;
    p = put_ef(p, NTS_EF_UNIQUE_ID, uid, NTS_NTP_UID_LEN);
    p = put_ef(p, NTS_EF_COOKIE, cookie, cookie_len);
    for (unsigned i = 0; i < placeholders; i++)
        p = put_ef(p, NTS_EF_COOKIE_PLACEHOLDER, NULL, cookie_len);   // zeros, same length

    // Authenticator: nonce length, ciphertext length, nonce, ciphertext (the tag alone: the
    // request has nothing to encrypt). The associated data is everything before this field.
    size_t ad_len = (size_t)(p - buf);
    put_u16(p, NTS_EF_AUTHENTICATOR);
    put_u16(p + 2, (uint16_t)auth_ef);
    put_u16(p + 4, NTS_NTP_NONCE_LEN);
    put_u16(p + 6, NTS_NTP_TAG_LEN);
    memcpy(p + 8, nonce, NTS_NTP_NONCE_LEN);
    if (!c2s->seal(c2s->ctx, buf, ad_len, nonce, NTS_NTP_NONCE_LEN, NULL, 0,
                   p + 8 + NTS_NTP_NONCE_LEN))
        return 0;
    return need;
}

// Walk the extension fields in buf[off..len). Calls back per field; stops early if it returns
// false. Returns NULL if all fields are well formed, otherwise a reason.
typedef bool (*ef_fn)(void *arg, uint16_t type, const uint8_t *ef, size_t ef_len, size_t off);

static const char *walk_efs(const uint8_t *buf, size_t off, size_t len, ef_fn fn, void *arg) {
    while (off < len) {
        if (len - off < 4)
            return "truncated extension field";
        uint16_t type = get_u16(buf + off);
        uint16_t ef_len = get_u16(buf + off + 2);
        if (ef_len < 4 || ef_len % 4 || ef_len > len - off)
            return "malformed extension field";
        if (!fn(arg, type, buf + off, ef_len, off))
            return NULL;
        off += ef_len;
    }
    return NULL;
}

typedef struct {
    const uint8_t *uid;
    bool uid_ok;            // a matching Unique Identifier came before the authenticator
    size_t auth_off;        // 0 = no authenticator
    size_t auth_len;
} outer_scan_t;

static bool scan_outer(void *arg, uint16_t type, const uint8_t *ef, size_t ef_len, size_t off) {
    outer_scan_t *s = arg;
    if (type == NTS_EF_UNIQUE_ID && ef_len == 4 + NTS_NTP_UID_LEN &&
        !memcmp(ef + 4, s->uid, NTS_NTP_UID_LEN))
        s->uid_ok = true;
    if (type == NTS_EF_AUTHENTICATOR) {
        s->auth_off = off;
        s->auth_len = ef_len;
        return false;       // anything after it is unauthenticated and ignored
    }
    return true;
}

static bool scan_inner(void *arg, uint16_t type, const uint8_t *ef, size_t ef_len, size_t off) {
    (void)off;
    nts_ntp_reply_t *r = arg;
    size_t body = ef_len - 4;
    if (type == NTS_EF_COOKIE && body > 0 && body <= NTS_NTP_COOKIE_MAX &&
        r->cookie_count < NTS_NTP_MAX_COOKIES) {
        memcpy(r->cookies[r->cookie_count], ef + 4, body);
        r->cookie_len[r->cookie_count++] = (uint16_t)body;
    }
    return true;
}

nts_ntp_status_t nts_ntp_parse_reply(const uint8_t *pkt, size_t len, const uint8_t xmt[8],
                                     const uint8_t uid[NTS_NTP_UID_LEN], const nts_aead_t *s2c,
                                     nts_ntp_reply_t *out, const char **reason) {
    static uint8_t plain[NTS_NTP_PACKET_MAX];
    memset(out, 0, sizeof(*out));
    *reason = NULL;
    if (len < NTS_NTP_HEADER_LEN || len > NTS_NTP_PACKET_MAX) {
        *reason = "bad packet length";
        return NTS_NTP_INVALID;
    }
    if ((pkt[0] & 7) != 4 || ((pkt[0] >> 3) & 7) != 4) {
        *reason = "not an NTPv4 server reply";
        return NTS_NTP_INVALID;
    }
    if (memcmp(pkt + 24, xmt, 8)) {
        *reason = "not a reply to our request (origin timestamp)";
        return NTS_NTP_INVALID;
    }

    outer_scan_t s = {.uid = uid};
    const char *why = walk_efs(pkt, NTS_NTP_HEADER_LEN, len, scan_outer, &s);
    if (why) {
        *reason = why;
        return NTS_NTP_INVALID;
    }
    if (!s.uid_ok) {
        *reason = "Unique Identifier missing or wrong";
        return NTS_NTP_INVALID;
    }
    if (pkt[1] == 0) {                          // Kiss-o'-Death
        if (!memcmp(pkt + 12, "NTSN", 4))
            return NTS_NTP_NAK;                 // unauthenticated by design (RFC 8915 5.7)
        *reason = "kiss-o'-death";
        return NTS_NTP_INVALID;
    }
    if (!s.auth_off) {
        *reason = "no NTS authenticator";
        return NTS_NTP_INVALID;
    }

    // Authenticator body: nonce length, ciphertext length, nonce (padded), ciphertext (padded).
    const uint8_t *a = pkt + s.auth_off + 4;
    size_t body = s.auth_len - 4;
    if (body < 4) {
        *reason = "malformed authenticator";
        return NTS_NTP_INVALID;
    }
    size_t nlen = get_u16(a), clen = get_u16(a + 2);
    if (nlen == 0 || clen < NTS_NTP_TAG_LEN || 4 + pad4(nlen) + pad4(clen) > body ||
        clen - NTS_NTP_TAG_LEN > sizeof(plain)) {
        *reason = "malformed authenticator";
        return NTS_NTP_INVALID;
    }
    const uint8_t *nonce = a + 4, *ct = a + 4 + pad4(nlen);
    if (!s2c->open(s2c->ctx, pkt, s.auth_off, nonce, nlen, ct, clen, plain)) {
        *reason = "authentication failed";
        return NTS_NTP_INVALID;
    }
    why = walk_efs(plain, 0, clen - NTS_NTP_TAG_LEN, scan_inner, out);
    if (why) {
        *reason = why;
        return NTS_NTP_INVALID;
    }

    out->leap = pkt[0] >> 6;
    out->version = (pkt[0] >> 3) & 7;
    out->mode = pkt[0] & 7;
    out->stratum = pkt[1];
    out->poll = (int8_t)pkt[2];
    out->precision = (int8_t)pkt[3];
    out->root_delay = get_u32(pkt + 4);
    out->root_dispersion = get_u32(pkt + 8);
    memcpy(out->refid, pkt + 12, 4);
    out->reference = get_u64(pkt + 16);
    out->origin = get_u64(pkt + 24);
    out->receive = get_u64(pkt + 32);
    out->transmit = get_u64(pkt + 40);
    return NTS_NTP_OK;
}

int64_t nts_ntp_to_unix_us(uint64_t ntp) {
    int64_t sec = (int64_t)(ntp >> 32);
    if (sec < 0x80000000LL)
        sec += 0x100000000LL;                   // era 1: from 2036-02-07
    uint64_t frac = ntp & 0xffffffffu;
    return (sec - NTP_UNIX_DIFF) * 1000000 + (int64_t)((frac * 1000000) >> 32);
}

uint64_t nts_ntp_from_unix_us(int64_t us) {
    int64_t sec = us / 1000000, sub = us % 1000000;
    if (sub < 0) {
        sub += 1000000;
        sec--;
    }
    uint64_t ntp_sec = (uint64_t)(sec + NTP_UNIX_DIFF) & 0xffffffffu;
    uint64_t frac = ((uint64_t)sub << 32) / 1000000;
    return (ntp_sec << 32) | frac;
}

void nts_ntp_offset(int64_t t1, int64_t t2, int64_t t3, int64_t t4, int64_t *offset,
                    int64_t *delay) {
    *offset = ((t2 - t1) + (t3 - t4)) / 2;
    *delay = (t4 - t1) - (t3 - t2);
}
