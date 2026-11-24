// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/time/nts_ke.c: NTS-KE request encoding and response parsing (RFC 8915).

#include <stdint.h>

#include "time/nts_ke.h"
#include "test.h"

// --- response builder ----------------------------------------------------------------------------

static uint8_t msg[NTS_KE_RESPONSE_MAX + 64];
static size_t msg_len;

static void rec(unsigned type, bool critical, const void *body, uint16_t len) {
    uint16_t head = (uint16_t)(type | (critical ? 0x8000u : 0));
    msg[msg_len++] = (uint8_t)(head >> 8);
    msg[msg_len++] = (uint8_t)head;
    msg[msg_len++] = (uint8_t)(len >> 8);
    msg[msg_len++] = (uint8_t)len;
    if (len)
        memcpy(msg + msg_len, body, len);
    msg_len += len;
}

static void rec_u16(unsigned type, bool critical, uint16_t v) {
    uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    rec(type, critical, b, 2);
}

// A typical good response: protocol, AEAD, three cookies, End of Message.
static void good_response(bool with_end) {
    msg_len = 0;
    rec_u16(NTS_KE_NEXT_PROTOCOL, true, NTS_PROTOCOL_NTPV4);
    rec_u16(NTS_KE_AEAD, false, NTS_AEAD_AES_SIV_CMAC_256);
    for (int i = 0; i < 3; i++) {
        uint8_t cookie[100];
        memset(cookie, 0xa0 + i, sizeof(cookie));
        rec(NTS_KE_COOKIE, false, cookie, sizeof(cookie));
    }
    if (with_end)
        rec(NTS_KE_END, true, NULL, 0);
}

static nts_ke_result_t res;

static nts_ke_status_t parse(const char **why) {
    return nts_ke_parse(msg, msg_len, &res, why);
}

// --- tests ---------------------------------------------------------------------------------------

TEST(request_bytes) {
    uint8_t buf[32];
    const uint8_t want[] = {
        0x80, 0x01, 0x00, 0x02, 0x00, 0x00,     // Next Protocol (critical): NTPv4
        0x00, 0x04, 0x00, 0x02, 0x00, 0x0f,     // AEAD: AES-SIV-CMAC-256 (15)
        0x80, 0x00, 0x00, 0x00,                 // End of Message (critical)
    };
    CHECK_EQ(nts_ke_build_request(buf, sizeof(buf)), sizeof(want));
    CHECK_MEM(buf, want, sizeof(want));
    CHECK_EQ(nts_ke_build_request(buf, 15), 0);
}

TEST(good_response_parses) {
    const char *why = "unset";
    good_response(true);
    CHECK_EQ(parse(&why), NTS_KE_OK);
    CHECK(why == NULL);
    CHECK_EQ(res.aead, NTS_AEAD_AES_SIV_CMAC_256);
    CHECK_EQ(res.cookie_count, 3);
    CHECK_EQ(res.cookie_len[2], 100);
    CHECK_EQ(res.cookies[2][99], 0xa2);
    CHECK_EQ(res.server[0], 0);
    CHECK_EQ(res.port, 0);
}

TEST(server_and_port_records) {
    const char *why;
    good_response(false);
    rec(NTS_KE_NTPV4_SERVER, false, "ntp1.example.net", 16);
    rec_u16(NTS_KE_NTPV4_PORT, false, 1123);
    rec(NTS_KE_END, true, NULL, 0);
    CHECK_EQ(parse(&why), NTS_KE_OK);
    CHECK(!strcmp(res.server, "ntp1.example.net"));
    CHECK_EQ(res.port, 1123);
}

TEST(every_prefix_needs_more) {
    // Any truncation of a valid response (as it arrives over TCP) asks for more data.
    const char *why;
    good_response(true);
    size_t full = msg_len;
    for (size_t n = 0; n < full; n++) {
        msg_len = n;
        if (parse(&why) != NTS_KE_NEED_MORE) {
            CHECK_EQ(n, (size_t)-1);
            return;
        }
    }
    msg_len = full;
    CHECK_EQ(parse(&why), NTS_KE_OK);
}

TEST(unknown_records) {
    const char *why;
    good_response(false);
    rec_u16(0x1234, false, 7);                  // unknown, not critical: ignored
    rec(NTS_KE_END, true, NULL, 0);
    CHECK_EQ(parse(&why), NTS_KE_OK);

    good_response(false);
    rec_u16(0x1234, true, 7);                   // unknown and critical: refused
    rec(NTS_KE_END, true, NULL, 0);
    CHECK_EQ(parse(&why), NTS_KE_INVALID);
    CHECK(!strcmp(why, "unknown critical record"));
}

TEST(error_and_warning_refused) {
    const char *why;
    msg_len = 0;
    rec_u16(NTS_KE_ERROR, true, 1);             // Bad Request
    rec(NTS_KE_END, true, NULL, 0);
    CHECK_EQ(parse(&why), NTS_KE_INVALID);
    CHECK_EQ(res.error_code, 1);

    good_response(false);
    rec_u16(NTS_KE_WARNING, true, 5);
    rec(NTS_KE_END, true, NULL, 0);
    CHECK_EQ(parse(&why), NTS_KE_INVALID);
    CHECK_EQ(res.warning_code, 5);
}

TEST(required_records) {
    const char *why;
    uint8_t cookie[10] = {1};

    msg_len = 0;                                // no Next Protocol
    rec_u16(NTS_KE_AEAD, false, NTS_AEAD_AES_SIV_CMAC_256);
    rec(NTS_KE_COOKIE, false, cookie, sizeof(cookie));
    rec(NTS_KE_END, true, NULL, 0);
    CHECK_EQ(parse(&why), NTS_KE_INVALID);
    CHECK(!strcmp(why, "no Next Protocol record"));

    msg_len = 0;                                // no AEAD
    rec_u16(NTS_KE_NEXT_PROTOCOL, true, NTS_PROTOCOL_NTPV4);
    rec(NTS_KE_COOKIE, false, cookie, sizeof(cookie));
    rec(NTS_KE_END, true, NULL, 0);
    CHECK_EQ(parse(&why), NTS_KE_INVALID);
    CHECK(!strcmp(why, "no AEAD record"));

    msg_len = 0;                                // no cookies
    rec_u16(NTS_KE_NEXT_PROTOCOL, true, NTS_PROTOCOL_NTPV4);
    rec_u16(NTS_KE_AEAD, false, NTS_AEAD_AES_SIV_CMAC_256);
    rec(NTS_KE_END, true, NULL, 0);
    CHECK_EQ(parse(&why), NTS_KE_INVALID);
    CHECK(!strcmp(why, "no cookies"));
}

TEST(wrong_choices_refused) {
    const char *why;
    uint8_t cookie[10] = {1};

    msg_len = 0;                                // empty protocol list: nothing accepted
    rec(NTS_KE_NEXT_PROTOCOL, true, NULL, 0);
    CHECK_EQ(parse(&why), NTS_KE_INVALID);
    CHECK(!strcmp(why, "server didn't choose NTPv4"));

    msg_len = 0;                                // a different AEAD
    rec_u16(NTS_KE_NEXT_PROTOCOL, true, NTS_PROTOCOL_NTPV4);
    rec_u16(NTS_KE_AEAD, false, 30);
    rec(NTS_KE_COOKIE, false, cookie, sizeof(cookie));
    rec(NTS_KE_END, true, NULL, 0);
    CHECK_EQ(parse(&why), NTS_KE_INVALID);
    CHECK(!strcmp(why, "server didn't choose AES-SIV-CMAC-256"));

    good_response(false);                       // duplicate protocol record
    rec_u16(NTS_KE_NEXT_PROTOCOL, true, NTS_PROTOCOL_NTPV4);
    rec(NTS_KE_END, true, NULL, 0);
    CHECK_EQ(parse(&why), NTS_KE_INVALID);
}

TEST(malformed_records) {
    const char *why;
    uint8_t big[NTS_KE_COOKIE_MAX + 1] = {0};

    good_response(false);
    rec(NTS_KE_COOKIE, false, big, sizeof(big));
    rec(NTS_KE_END, true, NULL, 0);
    CHECK_EQ(parse(&why), NTS_KE_INVALID);
    CHECK(!strcmp(why, "cookie of unsupported size"));

    good_response(false);
    rec(NTS_KE_NTPV4_SERVER, false, "a\0b", 3);
    rec(NTS_KE_END, true, NULL, 0);
    CHECK_EQ(parse(&why), NTS_KE_INVALID);

    good_response(false);
    rec_u16(NTS_KE_NTPV4_PORT, false, 0);
    rec(NTS_KE_END, true, NULL, 0);
    CHECK_EQ(parse(&why), NTS_KE_INVALID);

    good_response(false);
    rec_u16(NTS_KE_END, true, 0);               // End of Message with a body
    CHECK_EQ(parse(&why), NTS_KE_INVALID);

    good_response(true);
    msg[msg_len++] = 0;                         // trailing data
    CHECK_EQ(parse(&why), NTS_KE_INVALID);
    CHECK(!strcmp(why, "data after End of Message"));

    // A record claiming more than the response limit is refused, not waited for.
    msg_len = 0;
    msg[msg_len++] = 0x00;
    msg[msg_len++] = NTS_KE_COOKIE;
    msg[msg_len++] = 0xff;
    msg[msg_len++] = 0xff;
    CHECK_EQ(parse(&why), NTS_KE_INVALID);
    CHECK(!strcmp(why, "response too long"));
}

TEST(extra_cookies_ignored) {
    const char *why;
    good_response(false);
    for (int i = 0; i < 10; i++) {
        uint8_t cookie[50];
        memset(cookie, i, sizeof(cookie));
        rec(NTS_KE_COOKIE, false, cookie, sizeof(cookie));
    }
    rec(NTS_KE_END, true, NULL, 0);
    CHECK_EQ(parse(&why), NTS_KE_OK);
    CHECK_EQ(res.cookie_count, NTS_KE_MAX_COOKIES);
}

TEST(exporter_context) {
    uint8_t ctx[5];
    nts_ke_exporter_context(NTS_AEAD_AES_SIV_CMAC_256, false, ctx);
    const uint8_t c2s[5] = {0x00, 0x00, 0x00, 0x0f, 0x00};
    CHECK_MEM(ctx, c2s, 5);
    nts_ke_exporter_context(NTS_AEAD_AES_SIV_CMAC_256, true, ctx);
    const uint8_t s2c[5] = {0x00, 0x00, 0x00, 0x0f, 0x01};
    CHECK_MEM(ctx, s2c, 5);
}

TEST_MAIN(
    RUN(request_bytes);
    RUN(good_response_parses);
    RUN(server_and_port_records);
    RUN(every_prefix_needs_more);
    RUN(unknown_records);
    RUN(error_and_warning_refused);
    RUN(required_records);
    RUN(wrong_choices_refused);
    RUN(malformed_records);
    RUN(extra_cookies_ignored);
    RUN(exporter_context);
)
