// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The pure part of `ping`: see ping_msg.h.

#include "net/ping_msg.h"

#include <stdio.h>
#include <string.h>

#define ICMP_ECHO_REPLY   0
#define ICMP_UNREACHABLE  3
#define ICMP_ECHO_REQUEST 8
#define ICMP_TIME_EXCEEDED 11
#define IP_PROTO_ICMP     1

static const char usage[] = "usage: ping <host> [<count>]";

// A name or an IPv4 address: letters, digits, '-', '.' and '_' (found in some names on a LAN).
// lwIP's DNS client takes it from there.
static bool host_ok(const char *h) {
    size_t n = strlen(h);
    if (n == 0 || n > PING_HOST_MAX)
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = h[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '.' || c == '_'))
            return false;
    }
    return true;
}

// The next word of *s, ended in place; NULL if there is none.
static char *next_word(char **s) {
    char *p = *s;
    while (*p == ' ')
        p++;
    if (!*p)
        return NULL;
    char *w = p;
    while (*p && *p != ' ')
        p++;
    if (*p)
        *p++ = '\0';
    *s = p;
    return w;
}

const char *ping_parse_args(char *args, char **host, unsigned *count) {
    if (!args)
        return usage;
    char *h = next_word(&args);
    char *c = h ? next_word(&args) : NULL;
    if (!h || (c && next_word(&args)))
        return usage;
    if (!host_ok(h))
        return "ping: bad host name (a name or an IPv4 address)";
    unsigned n = PING_COUNT_DEFAULT;
    if (c) {
        // Digits only (no sign, no spaces), at most 5 of them, so no overflow below.
        size_t len = strlen(c);
        if (len > 5 || strspn(c, "0123456789") != len)
            return "ping: the count is a number from 1 to 65535";
        n = 0;
        for (size_t i = 0; i < len; i++)
            n = n * 10 + (unsigned)(c[i] - '0');
        if (n < 1 || n > PING_COUNT_MAX)
            return "ping: the count is a number from 1 to 65535";
    }
    *host = h;
    *count = n;
    return NULL;
}

uint16_t ping_checksum(const uint8_t *p, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < n; i += 2)
        sum += (uint32_t)p[i] << 8 | p[i + 1];
    if (n & 1)
        sum += (uint32_t)p[n - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint8_t data_byte(size_t i) {
    return (uint8_t)('a' + i % 26);
}

void ping_build_request(uint8_t *buf, uint16_t id, uint16_t seq) {
    buf[0] = ICMP_ECHO_REQUEST;
    buf[1] = 0;
    buf[2] = buf[3] = 0;                        // the checksum, computed with these as 0
    buf[4] = (uint8_t)(id >> 8);
    buf[5] = (uint8_t)id;
    buf[6] = (uint8_t)(seq >> 8);
    buf[7] = (uint8_t)seq;
    for (size_t i = 0; i < PING_DATA_SIZE; i++)
        buf[8 + i] = data_byte(i);
    uint16_t c = ping_checksum(buf, PING_ICMP_SIZE);
    buf[2] = (uint8_t)(c >> 8);
    buf[3] = (uint8_t)c;
}

// The ICMP part of pkt, an IPv4 packet of len bytes, if its header and length hold together and
// it is ICMP; NULL if not. Sets *icmp_len.
static const uint8_t *icmp_part(const uint8_t *pkt, size_t len, size_t *icmp_len) {
    if (len < 20 || pkt[0] >> 4 != 4)
        return NULL;
    size_t ihl = (size_t)(pkt[0] & 0x0f) * 4;
    if (ihl < 20 || len < ihl + 8 || ((size_t)pkt[2] << 8 | pkt[3]) != len ||
        pkt[9] != IP_PROTO_ICMP)
        return NULL;
    *icmp_len = len - ihl;
    return pkt + ihl;
}

bool ping_parse_reply(const uint8_t *pkt, size_t len, uint16_t id, uint16_t *seq, uint8_t *ttl) {
    size_t n;
    const uint8_t *icmp = icmp_part(pkt, len, &n);
    if (!icmp || n != PING_ICMP_SIZE || icmp[0] != ICMP_ECHO_REPLY || icmp[1] != 0 ||
        (uint16_t)(icmp[4] << 8 | icmp[5]) != id || ping_checksum(icmp, n) != 0)
        return false;
    for (size_t i = 0; i < PING_DATA_SIZE; i++)
        if (icmp[8 + i] != data_byte(i))
            return false;
    *seq = (uint16_t)(icmp[6] << 8 | icmp[7]);
    *ttl = pkt[8];
    return true;
}

bool ping_parse_error(const uint8_t *pkt, size_t len, uint16_t id, ping_error_t *e) {
    size_t n;
    const uint8_t *icmp = icmp_part(pkt, len, &n);
    if (!icmp || (icmp[0] != ICMP_UNREACHABLE && icmp[0] != ICMP_TIME_EXCEEDED) ||
        ping_checksum(icmp, n) != 0)
        return false;
    // The quoted datagram: its IP header, then at least 8 bytes of our echo request.
    const uint8_t *q = icmp + 8;
    size_t qn = n - 8;
    if (qn < 20 || q[0] >> 4 != 4)
        return false;
    size_t qihl = (size_t)(q[0] & 0x0f) * 4;
    if (qihl < 20 || qn < qihl + 8 || q[9] != IP_PROTO_ICMP)
        return false;
    const uint8_t *req = q + qihl;
    if (req[0] != ICMP_ECHO_REQUEST || req[1] != 0 || (uint16_t)(req[4] << 8 | req[5]) != id)
        return false;
    e->seq = (uint16_t)(req[6] << 8 | req[7]);
    e->type = icmp[0];
    e->code = icmp[1];
    return true;
}

void ping_error_text(uint8_t type, uint8_t code, char out[40]) {
    static const char *const unreachable[16] = {
        [0] = "network unreachable",
        [1] = "host unreachable",
        [2] = "protocol unreachable",
        [3] = "port unreachable",
        [4] = "fragmentation needed",
        [5] = "source route failed",
        [6] = "network unknown",
        [7] = "host unknown",
        [9] = "network prohibited",
        [10] = "host prohibited",
        [13] = "administratively prohibited",
    };
    static const char *const exceeded[2] = {
        [0] = "time to live exceeded",
        [1] = "reassembly time exceeded",
    };
    const char *name = NULL;
    if (type == ICMP_UNREACHABLE && code < 16)
        name = unreachable[code];
    else if (type == ICMP_TIME_EXCEEDED && code < 2)
        name = exceeded[code];
    if (name)
        snprintf(out, 40, "%s", name);
    else
        snprintf(out, 40, "%s (code %u)",
                 type == ICMP_TIME_EXCEEDED ? "time exceeded" : "destination unreachable", code);
}

void ping_stats_reply(ping_stats_t *s, uint32_t rtt_us) {
    if (!s->received || rtt_us < s->min_us)
        s->min_us = rtt_us;
    if (!s->received || rtt_us > s->max_us)
        s->max_us = rtt_us;
    s->sum_us += rtt_us;
    s->received++;
}

void ping_ms_text(uint32_t us, char out[16]) {
    uint32_t tenths = (uint32_t)(((uint64_t)us + 50) / 100);
    snprintf(out, 16, "%lu.%lu", (unsigned long)(tenths / 10), (unsigned long)(tenths % 10));
}

void ping_loss_text(const ping_stats_t *s, char out[8]) {
    out[0] = '\0';
    if (!s->sent)
        return;
    uint32_t lost = s->received < s->sent ? s->sent - s->received : 0;
    uint32_t pct = (uint32_t)(((uint64_t)lost * 100 + s->sent / 2) / s->sent);
    if (lost && pct == 0)
        strcpy(out, "<1%");
    else if (lost < s->sent && pct == 100)
        strcpy(out, ">99%");
    else
        snprintf(out, 8, "%u%%", (unsigned char)pct);  // 0 to 100 (the type tells the compiler)
}

int ping_summary(const ping_stats_t *s, char *out, size_t size) {
    char loss[16] = "", icmp[32] = "", errors[24] = "", rtt[80] = "";
    if (s->sent) {
        char pct[8];
        ping_loss_text(s, pct);
        snprintf(loss, sizeof(loss), ", %s loss", pct);
    }
    if (s->icmp_errors)
        snprintf(icmp, sizeof(icmp), ", %lu ICMP error%s", (unsigned long)s->icmp_errors,
                 s->icmp_errors == 1 ? "" : "s");
    if (s->not_sent)
        snprintf(errors, sizeof(errors), ", %lu not sent", (unsigned long)s->not_sent);
    if (s->received) {
        char lo[16], avg[16], hi[16];
        ping_ms_text(s->min_us, lo);
        ping_ms_text((uint32_t)((s->sum_us + s->received / 2) / s->received), avg);
        ping_ms_text(s->max_us, hi);
        snprintf(rtt, sizeof(rtt), "; round trip min/avg/max %s/%s/%s ms", lo, avg, hi);
    }
    return snprintf(out, size, "%lu sent, %lu received%s%s%s%s", (unsigned long)s->sent,
                    (unsigned long)s->received, loss, icmp, errors, rtt);
}
