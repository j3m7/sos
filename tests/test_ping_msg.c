// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/net/ping_msg.c: ping's arguments, echo requests and replies, statistics.

#include "net/ping_msg.h"
#include "test.h"

// --- arguments -----------------------------------------------------------------------------------

static const char *parse(const char *line, char **host, unsigned *count) {
    static char buf[512];
    if (!line)
        return ping_parse_args(NULL, host, count);
    strcpy(buf, line);
    return ping_parse_args(buf, host, count);
}

TEST(args_host_and_count) {
    char *host = NULL;
    unsigned count = 0;
    CHECK(parse("example.com", &host, &count) == NULL);
    CHECK(!strcmp(host, "example.com"));
    CHECK_EQ(count, PING_COUNT_DEFAULT);
    CHECK(parse("192.168.1.1 10", &host, &count) == NULL);
    CHECK(!strcmp(host, "192.168.1.1"));
    CHECK_EQ(count, 10);
    CHECK(parse("my_host-2.lan   1  ", &host, &count) == NULL);   // extra spaces
    CHECK(!strcmp(host, "my_host-2.lan"));
    CHECK_EQ(count, 1);
    CHECK(parse("h 65535", &host, &count) == NULL);
    CHECK_EQ(count, 65535);
}

TEST(args_refused) {
    char *host;
    unsigned count;
    CHECK(parse(NULL, &host, &count) != NULL);
    CHECK(parse("", &host, &count) != NULL);
    CHECK(parse("   ", &host, &count) != NULL);
    CHECK(parse("h 3 x", &host, &count) != NULL);       // a third word
    CHECK(parse("h 0", &host, &count) != NULL);
    CHECK(parse("h 65536", &host, &count) != NULL);
    CHECK(parse("h 100000", &host, &count) != NULL);
    CHECK(parse("h 0000010", &host, &count) != NULL);   // more than 5 digits
    CHECK(parse("h -1", &host, &count) != NULL);
    CHECK(parse("h +1", &host, &count) != NULL);
    CHECK(parse("h 1x", &host, &count) != NULL);
    CHECK(parse("bad!host", &host, &count) != NULL);
    CHECK(parse("host/1", &host, &count) != NULL);
    CHECK(parse("h\x1b[2J", &host, &count) != NULL);    // terminal escapes don't get printed back
    char name[300];
    memset(name, 'a', sizeof(name));
    name[PING_HOST_MAX] = '\0';
    CHECK(parse(name, &host, &count) == NULL);          // 253 characters
    name[PING_HOST_MAX] = 'a';
    name[PING_HOST_MAX + 1] = '\0';
    CHECK(parse(name, &host, &count) != NULL);          // 254
}

// --- packets -------------------------------------------------------------------------------------

// RFC 1071's example: the sum of 0001 f203 f4f5 f6f7 folds to ddf2.
TEST(checksum_rfc1071) {
    const uint8_t b[] = { 0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7 };
    CHECK_EQ(ping_checksum(b, sizeof(b)), 0x220d);
    const uint8_t odd[] = { 0x00, 0x01, 0xf2 };       // the last byte is padded with a zero
    CHECK_EQ(ping_checksum(odd, sizeof(odd)), (uint16_t)~(0x0001 + 0xf200));
}

TEST(request) {
    uint8_t r[PING_ICMP_SIZE];
    ping_build_request(r, 0xbeef, 0x0102);
    CHECK_EQ(r[0], 8);
    CHECK_EQ(r[1], 0);
    CHECK_EQ(r[4], 0xbe);
    CHECK_EQ(r[5], 0xef);
    CHECK_EQ(r[6], 0x01);
    CHECK_EQ(r[7], 0x02);
    CHECK_EQ(ping_checksum(r, sizeof(r)), 0);           // its checksum is right
    CHECK_EQ(r[8], 'a');
    CHECK_EQ(r[8 + 26], 'a');
}

// What a host sends back for request (id, seq): an IP header of `ihl` bytes, then the request
// with its type changed to 0 and the checksum redone. Returns the packet's length.
static size_t reply(uint8_t *pkt, size_t ihl, uint16_t id, uint16_t seq) {
    size_t len = ihl + PING_ICMP_SIZE;
    memset(pkt, 0, ihl);
    pkt[0] = (uint8_t)(0x40 | ihl / 4);
    pkt[2] = (uint8_t)(len >> 8);
    pkt[3] = (uint8_t)len;
    pkt[8] = 57;                                        // TTL
    pkt[9] = 1;                                         // ICMP
    uint8_t *icmp = pkt + ihl;
    ping_build_request(icmp, id, seq);
    icmp[0] = 0;
    icmp[2] = icmp[3] = 0;
    uint16_t c = ping_checksum(icmp, PING_ICMP_SIZE);
    icmp[2] = (uint8_t)(c >> 8);
    icmp[3] = (uint8_t)c;
    return len;
}

TEST(reply_accepted) {
    uint8_t p[PING_REPLY_MAX];
    uint16_t seq = 0;
    uint8_t ttl = 0;
    size_t n = reply(p, 20, 0x1234, 7);
    CHECK(ping_parse_reply(p, n, 0x1234, &seq, &ttl));
    CHECK_EQ(seq, 7);
    CHECK_EQ(ttl, 57);
    n = reply(p, 60, 0x1234, 65535);                    // the longest header (IP options)
    CHECK_EQ(n, PING_REPLY_MAX);
    CHECK(ping_parse_reply(p, n, 0x1234, &seq, &ttl));
    CHECK_EQ(seq, 65535);
}

TEST(reply_refused) {
    uint8_t p[PING_REPLY_MAX + 8];
    uint16_t seq = 99;
    uint8_t ttl = 99;
    size_t n = reply(p, 20, 0x1234, 7);
    CHECK(!ping_parse_reply(p, n, 0x1235, &seq, &ttl));     // another run's identifier
    CHECK(!ping_parse_reply(p, n - 1, 0x1234, &seq, &ttl)); // cut short
    CHECK(!ping_parse_reply(p, 10, 0x1234, &seq, &ttl));
    CHECK(!ping_parse_reply(p, 0, 0x1234, &seq, &ttl));
    CHECK(!ping_parse_reply(p, n + 1, 0x1234, &seq, &ttl)); // longer than the IP length says

    n = reply(p, 20, 0x1234, 7);
    p[20] = 8;                                              // a request (someone pinging us)
    CHECK(!ping_parse_reply(p, n, 0x1234, &seq, &ttl));
    n = reply(p, 20, 0x1234, 7);
    p[21] = 1;                                              // code 1
    CHECK(!ping_parse_reply(p, n, 0x1234, &seq, &ttl));
    n = reply(p, 20, 0x1234, 7);
    p[23] ^= 1;                                             // bad checksum
    CHECK(!ping_parse_reply(p, n, 0x1234, &seq, &ttl));
    n = reply(p, 20, 0x1234, 7);
    p[20 + 8 + 3] ^= 1;                                     // other data (checksum broken too)
    CHECK(!ping_parse_reply(p, n, 0x1234, &seq, &ttl));
    n = reply(p, 20, 0x1234, 7);
    p[9] = 17;                                              // UDP
    CHECK(!ping_parse_reply(p, n, 0x1234, &seq, &ttl));
    n = reply(p, 20, 0x1234, 7);
    p[0] = 0x65;                                            // version 6
    CHECK(!ping_parse_reply(p, n, 0x1234, &seq, &ttl));
    n = reply(p, 20, 0x1234, 7);
    p[0] = 0x44;                                            // header shorter than 20 bytes
    CHECK(!ping_parse_reply(p, n, 0x1234, &seq, &ttl));
    n = reply(p, 20, 0x1234, 7);
    p[3]++;                                                 // IP length disagrees
    CHECK(!ping_parse_reply(p, n, 0x1234, &seq, &ttl));

    // More data than we sent, with a correct checksum: not a reply to our request.
    n = reply(p, 20, 0x1234, 7);
    p[n] = 'g';
    p[n + 1] = 'h';
    n += 2;
    p[2] = (uint8_t)(n >> 8);
    p[3] = (uint8_t)n;
    p[22] = p[23] = 0;
    uint16_t c = ping_checksum(p + 20, n - 20);
    p[22] = (uint8_t)(c >> 8);
    p[23] = (uint8_t)c;
    CHECK(!ping_parse_reply(p, n, 0x1234, &seq, &ttl));

    CHECK_EQ(seq, 99);                                      // untouched when refused
    CHECK_EQ(ttl, 99);
}

// --- statistics ----------------------------------------------------------------------------------

TEST(ms_text) {
    char t[16];
    ping_ms_text(0, t);
    CHECK(!strcmp(t, "0.0"));
    ping_ms_text(49, t);
    CHECK(!strcmp(t, "0.0"));
    ping_ms_text(50, t);
    CHECK(!strcmp(t, "0.1"));
    ping_ms_text(4249, t);
    CHECK(!strcmp(t, "4.2"));
    ping_ms_text(4250, t);
    CHECK(!strcmp(t, "4.3"));
    ping_ms_text(1000000, t);
    CHECK(!strcmp(t, "1000.0"));
    ping_ms_text(UINT32_MAX, t);
    CHECK(!strcmp(t, "4294967.3"));
}

static ping_stats_t stats(uint32_t sent, uint32_t received) {
    ping_stats_t s = { 0 };
    s.sent = sent;
    s.received = received;
    return s;
}

TEST(loss_text) {
    char t[8];
    ping_stats_t s = stats(0, 0);
    ping_loss_text(&s, t);
    CHECK(!strcmp(t, ""));
    s = stats(4, 4);
    ping_loss_text(&s, t);
    CHECK(!strcmp(t, "0%"));
    s = stats(4, 3);
    ping_loss_text(&s, t);
    CHECK(!strcmp(t, "25%"));
    s = stats(3, 1);
    ping_loss_text(&s, t);
    CHECK(!strcmp(t, "67%"));
    s = stats(4, 0);
    ping_loss_text(&s, t);
    CHECK(!strcmp(t, "100%"));
    s = stats(1000, 999);
    ping_loss_text(&s, t);
    CHECK(!strcmp(t, "<1%"));
    s = stats(1000, 1);
    ping_loss_text(&s, t);
    CHECK(!strcmp(t, ">99%"));
    s = stats(65535, 0);
    ping_loss_text(&s, t);
    CHECK(!strcmp(t, "100%"));
}

TEST(summary) {
    char t[160];
    ping_stats_t s = { 0 };
    s.sent = 4;
    ping_stats_reply(&s, 6200);
    ping_stats_reply(&s, 3100);
    ping_stats_reply(&s, 2700);
    CHECK_EQ(s.received, 3);
    CHECK_EQ(s.min_us, 2700);
    CHECK_EQ(s.max_us, 6200);
    int n = ping_summary(&s, t, sizeof(t));
    CHECK(!strcmp(t, "4 sent, 3 received, 25% loss; round trip min/avg/max 2.7/4.0/6.2 ms"));
    CHECK_EQ(n, (int)strlen(t));

    s = stats(2, 0);
    s.errors = 1;
    ping_summary(&s, t, sizeof(t));
    CHECK(!strcmp(t, "2 sent, 0 received, 100% loss, 1 not sent"));

    s = stats(0, 0);
    ping_summary(&s, t, sizeof(t));
    CHECK(!strcmp(t, "0 sent, 0 received"));

    s = stats(1, 0);
    n = ping_summary(&s, t, 8);                         // cut to fit, still terminated
    CHECK(!strcmp(t, "1 sent,"));
    CHECK(n > 8);
}

TEST_MAIN(
    RUN(args_host_and_count);
    RUN(args_refused);
    RUN(checksum_rfc1071);
    RUN(request);
    RUN(reply_accepted);
    RUN(reply_refused);
    RUN(ms_text);
    RUN(loss_text);
    RUN(summary);
)
