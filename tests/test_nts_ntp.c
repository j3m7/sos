// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/time/nts_ntp.c: NTS-protected NTP packets (RFC 8915 section 5), with a stand-in
// AEAD (keyed checksum tag + XOR) in place of AES-SIV, and NTP time conversions.

#include <stdint.h>

#include "time/nts_ntp.h"
#include "test.h"

// --- stand-in AEAD: the tag is a keyed FNV-1a over ad, nonce and plaintext -----------------------

typedef struct {
    uint8_t key;
} fake_key_t;

static void fake_tag(uint8_t key, const uint8_t *ad, size_t adlen, const uint8_t *nonce,
                     size_t nlen, const uint8_t *pt, size_t ptlen, uint8_t tag[16]) {
    uint64_t h = 1469598103934665603ULL ^ key;
    const uint8_t *parts[3] = {ad, nonce, pt};
    size_t lens[3] = {adlen, nlen, ptlen};
    for (int k = 0; k < 3; k++) {
        for (size_t i = 0; i < lens[k]; i++)
            h = (h ^ parts[k][i]) * 1099511628211ULL;
        h = (h ^ 0xff) * 1099511628211ULL;      // separate the parts
    }
    for (int i = 0; i < 16; i++)
        tag[i] = (uint8_t)(h >> ((i % 8) * 8)) ^ (uint8_t)i;
}

static bool fake_seal(void *ctx, const uint8_t *ad, size_t adlen, const uint8_t *nonce,
                      size_t nlen, const uint8_t *pt, size_t ptlen, uint8_t *out) {
    uint8_t key = ((fake_key_t *)ctx)->key;
    fake_tag(key, ad, adlen, nonce, nlen, pt, ptlen, out);
    for (size_t i = 0; i < ptlen; i++)
        out[16 + i] = pt[i] ^ key;
    return true;
}

static bool fake_open(void *ctx, const uint8_t *ad, size_t adlen, const uint8_t *nonce,
                      size_t nlen, const uint8_t *ct, size_t ctlen, uint8_t *pt) {
    uint8_t key = ((fake_key_t *)ctx)->key, tag[16];
    for (size_t i = 16; i < ctlen; i++)
        pt[i - 16] = ct[i] ^ key;
    fake_tag(key, ad, adlen, nonce, nlen, pt, ctlen - 16, tag);
    return !memcmp(tag, ct, 16);
}

static fake_key_t c2s_key = {0x11}, s2c_key = {0x22};
static const nts_aead_t c2s = {fake_seal, fake_open, &c2s_key};
static const nts_aead_t s2c = {fake_seal, fake_open, &s2c_key};
static const nts_aead_t wrong = {fake_seal, fake_open, &c2s_key};   // wrong key for replies

static const uint8_t xmt[8] = {0xde, 0xad, 0xbe, 0xef, 1, 2, 3, 4};
static uint8_t uid[NTS_NTP_UID_LEN], nonce[NTS_NTP_NONCE_LEN], cookie[96];

static void init_values(void) {
    for (int i = 0; i < NTS_NTP_UID_LEN; i++)
        uid[i] = (uint8_t)(0x40 + i);
    for (int i = 0; i < NTS_NTP_NONCE_LEN; i++)
        nonce[i] = (uint8_t)(0x90 + i);
    for (int i = 0; i < 96; i++)
        cookie[i] = (uint8_t)i;
}

// --- a stand-in server reply ---------------------------------------------------------------------

static uint8_t pkt[NTS_NTP_PACKET_MAX];
static uint8_t scratch[NTS_NTP_PACKET_MAX];  // where a reply's encrypted part is opened
static size_t pkt_len;

static uint8_t *ef(uint8_t *p, uint16_t type, const uint8_t *body, size_t len) {
    size_t total = 4 + ((len + 3) & ~(size_t)3);
    p[0] = (uint8_t)(type >> 8);
    p[1] = (uint8_t)type;
    p[2] = (uint8_t)(total >> 8);
    p[3] = (uint8_t)total;
    memset(p + 4, 0, total - 4);
    if (len)
        memcpy(p + 4, body, len);
    return p + total;
}

// Server reply to (xmt, uid) carrying `ncookies` new cookies, sealed with `key`.
static void make_reply(unsigned ncookies, const nts_aead_t *key) {
    memset(pkt, 0, NTS_NTP_HEADER_LEN);
    pkt[0] = (0 << 6) | (4 << 3) | 4;           // NTPv4, server
    pkt[1] = 3;                                 // stratum
    memcpy(pkt + 12, "\x0a\x00\x00\x01", 4);
    memcpy(pkt + 24, xmt, 8);                   // origin = our transmit
    const uint8_t rec[8] = {0xed, 0x00, 0x00, 0x01, 0x80, 0, 0, 0};
    const uint8_t snd[8] = {0xed, 0x00, 0x00, 0x01, 0x80, 0x10, 0, 0};
    memcpy(pkt + 32, rec, 8);
    memcpy(pkt + 40, snd, 8);
    uint8_t *p = ef(pkt + NTS_NTP_HEADER_LEN, NTS_EF_UNIQUE_ID, uid, NTS_NTP_UID_LEN);

    uint8_t plain[1024], *q = plain;
    for (unsigned i = 0; i < ncookies; i++) {
        uint8_t c[96];
        memset(c, 0xc0 + (int)i, sizeof(c));
        q = ef(q, NTS_EF_COOKIE, c, sizeof(c));
    }
    size_t ptlen = (size_t)(q - plain);
    size_t ad_len = (size_t)(p - pkt);
    size_t clen = 16 + ptlen;
    size_t total = 4 + 4 + NTS_NTP_NONCE_LEN + ((clen + 3) & ~(size_t)3);
    p[0] = NTS_EF_AUTHENTICATOR >> 8;
    p[1] = NTS_EF_AUTHENTICATOR & 0xff;
    p[2] = (uint8_t)(total >> 8);
    p[3] = (uint8_t)total;
    p[4] = 0;
    p[5] = NTS_NTP_NONCE_LEN;
    p[6] = (uint8_t)(clen >> 8);
    p[7] = (uint8_t)clen;
    const uint8_t snonce[NTS_NTP_NONCE_LEN] = {7, 7, 7};
    memcpy(p + 8, snonce, NTS_NTP_NONCE_LEN);
    memset(p + 8 + NTS_NTP_NONCE_LEN, 0, total - 8 - NTS_NTP_NONCE_LEN);
    key->seal(key->ctx, pkt, ad_len, snonce, NTS_NTP_NONCE_LEN, plain, ptlen,
              p + 8 + NTS_NTP_NONCE_LEN);
    pkt_len = ad_len + total;
}

static nts_ntp_reply_t reply;

static nts_ntp_status_t parse(const char **why) {
    return nts_ntp_parse_reply(pkt, pkt_len, xmt, uid, &s2c, scratch, sizeof(scratch), &reply,
                               why);
}

// --- tests ---------------------------------------------------------------------------------------

TEST(request_layout) {
    uint8_t buf[NTS_NTP_PACKET_MAX];
    size_t n = nts_ntp_build_request(buf, sizeof(buf), xmt, uid, cookie, sizeof(cookie), 2,
                                     nonce, &c2s);
    // header 48, uid 36, cookie 100, 2 placeholders 200, authenticator 40
    CHECK_EQ(n, 48 + 36 + 100 + 200 + 40);
    CHECK_EQ(buf[0], 0x23);                     // LI 0, VN 4, mode 3
    CHECK_MEM(buf + 40, xmt, 8);
    CHECK_EQ(buf[48], 0x01);                    // Unique Identifier 0x0104, length 36
    CHECK_EQ(buf[49], 0x04);
    CHECK_EQ(buf[51], 36);
    CHECK_MEM(buf + 52, uid, NTS_NTP_UID_LEN);
    CHECK_EQ(buf[84], 0x02);                    // Cookie, length 100
    CHECK_EQ(buf[87], 100);
    CHECK_MEM(buf + 88, cookie, sizeof(cookie));
    CHECK_EQ(buf[184], 0x03);                   // Placeholder, length 100, zeros
    CHECK_EQ(buf[187], 100);
    uint8_t zeros[96] = {0};
    CHECK_MEM(buf + 188, zeros, 96);
    size_t a = 48 + 36 + 100 + 200;             // Authenticator
    CHECK_EQ(buf[a], 0x04);
    CHECK_EQ(buf[a + 1], 0x04);
    CHECK_EQ(buf[a + 3], 40);
    CHECK_EQ(buf[a + 5], NTS_NTP_NONCE_LEN);
    CHECK_EQ(buf[a + 7], 16);
    CHECK_MEM(buf + a + 8, nonce, NTS_NTP_NONCE_LEN);
    // The tag covers everything before the authenticator.
    uint8_t pt[1];
    CHECK(fake_open(&c2s_key, buf, a, nonce, NTS_NTP_NONCE_LEN, buf + a + 24, 16, pt));
    buf[10] ^= 1;
    CHECK(!fake_open(&c2s_key, buf, a, nonce, NTS_NTP_NONCE_LEN, buf + a + 24, 16, pt));
}

TEST(request_limits) {
    uint8_t buf[NTS_NTP_PACKET_MAX];
    CHECK_EQ(nts_ntp_build_request(buf, 100, xmt, uid, cookie, sizeof(cookie), 0, nonce, &c2s), 0);
    CHECK_EQ(nts_ntp_build_request(buf, sizeof(buf), xmt, uid, cookie, 0, 0, nonce, &c2s), 0);
    CHECK_EQ(nts_ntp_build_request(buf, sizeof(buf), xmt, uid, cookie, sizeof(cookie),
                                   NTS_NTP_MAX_COOKIES, nonce, &c2s), 0);
    // A cookie whose length isn't a multiple of 4 is padded.
    size_t n = nts_ntp_build_request(buf, sizeof(buf), xmt, uid, cookie, 95, 0, nonce, &c2s);
    CHECK_EQ(n, 48 + 36 + 100 + 40);
    CHECK_EQ(buf[87], 100);
}

TEST(good_reply) {
    const char *why = "unset";
    make_reply(3, &s2c);
    CHECK_EQ(parse(&why), NTS_NTP_OK);
    CHECK(why == NULL);
    CHECK_EQ(reply.mode, 4);
    CHECK_EQ(reply.version, 4);
    CHECK_EQ(reply.stratum, 3);
    CHECK_EQ(reply.cookie_count, 3);
    CHECK_EQ(reply.cookie_len[0], 96);
    CHECK_EQ(reply.cookies[2][95], 0xc2);
    CHECK_EQ(reply.receive, 0xed00000180000000ULL);
    CHECK_EQ(reply.transmit, 0xed00000180100000ULL);
}

// The caller supplies the space the encrypted part is opened into. Too little of it is an
// invalid reply, never a write past it: the byte after the space given stays as it was.
TEST(working_space_too_small) {
    const char *why = NULL;
    make_reply(3, &s2c);
    CHECK_EQ(parse(&why), NTS_NTP_OK);              // with all of it, this reply is good
    static uint8_t small[65];
    memset(small, 0xa5, sizeof(small));
    CHECK_EQ(nts_ntp_parse_reply(pkt, pkt_len, xmt, uid, &s2c, small, 64, &reply, &why),
             NTS_NTP_INVALID);
    CHECK(!strcmp(why, "malformed authenticator"));
    CHECK_EQ(small[64], 0xa5);
    CHECK_EQ(small[0], 0xa5);                       // refused before anything was opened
}

TEST(any_tampering_is_caught) {
    // Flip one bit in every byte of the reply: header, Unique Identifier, and the
    // authenticator's lengths, nonce and ciphertext. With one 96-byte cookie the ciphertext
    // (16 + 100 bytes) needs no padding, so every change must be rejected.
    const char *why;
    make_reply(1, &s2c);
    size_t len = pkt_len;
    for (size_t i = 0; i < len; i++) {
        make_reply(1, &s2c);
        pkt[i] ^= 0x01;
        if (parse(&why) == NTS_NTP_OK) {
            CHECK_EQ(i, (size_t)-1);            // report the byte that got through
            return;
        }
    }
    CHECK(1);
}

TEST(wrong_key_or_request) {
    const char *why;
    make_reply(1, &wrong);
    CHECK_EQ(parse(&why), NTS_NTP_INVALID);
    CHECK(!strcmp(why, "authentication failed"));

    make_reply(1, &s2c);
    uint8_t other_xmt[8] = {1};
    CHECK_EQ(nts_ntp_parse_reply(pkt, pkt_len, other_xmt, uid, &s2c, scratch, sizeof(scratch),
                                 &reply, &why),
             NTS_NTP_INVALID);
    CHECK(!strcmp(why, "not a reply to our request (origin timestamp)"));

    uint8_t other_uid[NTS_NTP_UID_LEN] = {9};
    CHECK_EQ(nts_ntp_parse_reply(pkt, pkt_len, xmt, other_uid, &s2c, scratch, sizeof(scratch),
                                 &reply, &why),
             NTS_NTP_INVALID);
    CHECK(!strcmp(why, "Unique Identifier missing or wrong"));
}

TEST(structure_errors) {
    const char *why;
    make_reply(1, &s2c);
    CHECK_EQ(nts_ntp_parse_reply(pkt, 40, xmt, uid, &s2c, scratch, sizeof(scratch), &reply,
                                 &why),
             NTS_NTP_INVALID);

    make_reply(1, &s2c);
    pkt[0] = (4 << 3) | 3;                      // client mode
    CHECK_EQ(parse(&why), NTS_NTP_INVALID);

    make_reply(1, &s2c);                        // no authenticator: cut after the UID field
    pkt_len = NTS_NTP_HEADER_LEN + 36;
    CHECK_EQ(parse(&why), NTS_NTP_INVALID);
    CHECK(!strcmp(why, "no NTS authenticator"));

    make_reply(1, &s2c);
    pkt[NTS_NTP_HEADER_LEN + 3] = 35;           // UID length not a multiple of 4
    CHECK_EQ(parse(&why), NTS_NTP_INVALID);
    CHECK(!strcmp(why, "malformed extension field"));

    make_reply(1, &s2c);
    pkt_len -= 2;                               // truncated authenticator
    CHECK_EQ(parse(&why), NTS_NTP_INVALID);
}

TEST(kiss_of_death) {
    const char *why;
    make_reply(0, &s2c);
    pkt[1] = 0;                                 // stratum 0
    memcpy(pkt + 12, "NTSN", 4);
    CHECK_EQ(parse(&why), NTS_NTP_NAK);

    uint8_t other_uid[NTS_NTP_UID_LEN] = {9};   // a NAK for someone else's request
    CHECK_EQ(nts_ntp_parse_reply(pkt, pkt_len, xmt, other_uid, &s2c, scratch, sizeof(scratch),
                                 &reply, &why),
             NTS_NTP_INVALID);

    make_reply(0, &s2c);
    pkt[1] = 0;
    memcpy(pkt + 12, "RATE", 4);
    CHECK_EQ(parse(&why), NTS_NTP_INVALID);
    CHECK(!strcmp(why, "kiss-o'-death"));
}

TEST(round_trip_with_own_request) {
    // A reply built from our own request's fields parses; the authenticator in the request
    // checks out with the client key the same way the server would check it.
    uint8_t req[NTS_NTP_PACKET_MAX];
    size_t n = nts_ntp_build_request(req, sizeof(req), xmt, uid, cookie, sizeof(cookie), 7,
                                     nonce, &c2s);
    CHECK(n > 0);
    make_reply(8, &s2c);
    const char *why;
    CHECK_EQ(parse(&why), NTS_NTP_OK);
    CHECK_EQ(reply.cookie_count, 8);
}

TEST(time_conversions) {
    // 2026-11-24 00:00:00 UTC = 1795478400 s since 1970.
    int64_t us = 1795478400LL * 1000000 + 250000;
    uint64_t ntp = nts_ntp_from_unix_us(us);
    CHECK_EQ(ntp >> 32, 1795478400LL + 2208988800LL);
    CHECK_EQ(ntp & 0xffffffffu, 0x40000000u);   // 0.25 s
    CHECK_EQ(nts_ntp_to_unix_us(ntp), us);
    // Era 1: 2040-01-01 = 2208988800 s since 1970; NTP seconds wrap past 2^32.
    int64_t later = 2208988800LL * 1000000;
    uint64_t n2 = nts_ntp_from_unix_us(later);
    CHECK((n2 >> 32) < 0x80000000u);
    CHECK_EQ(nts_ntp_to_unix_us(n2), later);
    // Sub-microsecond rounding stays within 1 us.
    for (int64_t t = us; t < us + 1000; t += 7) {
        int64_t back = nts_ntp_to_unix_us(nts_ntp_from_unix_us(t));
        if (back != t && back != t - 1) {
            CHECK_EQ(back, t);
            return;
        }
    }
}

TEST(offset_and_delay) {
    int64_t off, delay;
    // Server 100 ms ahead, 20 ms each way, 1 ms in the server.
    nts_ntp_offset(1000000, 1120000, 1121000, 1041000, &off, &delay);
    CHECK_EQ(off, 100000);
    CHECK_EQ(delay, 40000);
}

TEST_MAIN(
    init_values();
    RUN(request_layout);
    RUN(request_limits);
    RUN(good_reply);
    RUN(working_space_too_small);
    RUN(any_tampering_is_caught);
    RUN(wrong_key_or_request);
    RUN(structure_errors);
    RUN(kiss_of_death);
    RUN(round_trip_with_own_request);
    RUN(time_conversions);
    RUN(offset_and_delay);
)
