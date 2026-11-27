// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The pure part of `ping` (net/ping.c sends and receives): its arguments, ICMP echo requests,
// replies and errors as bytes, and the run's statistics as text.
//
// Pure C: no Pico SDK or lwIP dependency, host tested.

#ifndef SOS_PING_MSG_H
#define SOS_PING_MSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PING_DATA_SIZE      32                      // bytes after the 8-byte ICMP header
#define PING_ICMP_SIZE      (8 + PING_DATA_SIZE)    // an echo request, and so its reply
#define PING_REPLY_MAX      (60 + PING_ICMP_SIZE)   // the reply with the longest IPv4 header
#define PING_RECV_MAX       (60 + 8 + 60 + PING_ICMP_SIZE)  // the longest ICMP error about a
                                    // request: both IP headers at their longest, all of it quoted
#define PING_COUNT_DEFAULT  4
#define PING_COUNT_MAX      65535                   // sequence numbers are 16 bits
#define PING_HOST_MAX       253                     // the longest DNS name, as text

// `ping`'s arguments, "<host> [<count>]". Returns NULL and sets *host (pointing into args, which
// is modified) and *count; or the message to print. args may be NULL.
const char *ping_parse_args(char *args, char **host, unsigned *count);

// The Internet checksum (RFC 1071) of n bytes, to store big-endian. Over a message that already
// carries a correct checksum it is 0.
uint16_t ping_checksum(const uint8_t *p, size_t n);

// An echo request (ICMP type 8) with identifier `id` and sequence number `seq`, into buf, which
// holds PING_ICMP_SIZE bytes. Its data is a fixed pattern that the reply has to bring back.
void ping_build_request(uint8_t *buf, uint16_t id, uint16_t seq);

// Is pkt, an IPv4 packet of len bytes (header, then ICMP), the reply to a request built by
// ping_build_request() with identifier `id`? That is: an echo reply (type 0, code 0) of exactly
// PING_ICMP_SIZE bytes, with that identifier, the request's data and a correct ICMP checksum.
// Then sets *seq and *ttl (the reply's time to live). The IP header's checksum isn't checked:
// lwIP has done that.
bool ping_parse_reply(const uint8_t *pkt, size_t len, uint16_t id, uint16_t *seq, uint8_t *ttl);

// An ICMP error about one of our requests.
typedef struct {
    uint16_t seq;           // the request's
    uint8_t type;           // 3: destination unreachable, 11: time exceeded
    uint8_t code;
} ping_error_t;

// Is pkt, an IPv4 packet of len bytes, an ICMP error about a request built by
// ping_build_request() with identifier `id`? That is: destination unreachable (type 3) or time
// exceeded (type 11) with a correct ICMP checksum, quoting an IPv4 datagram whose protocol is
// ICMP and whose first 8 bytes of ICMP are an echo request (type 8, code 0) with that
// identifier. Then sets *e. Only the quoted IP header and those 8 bytes are read: every router
// quotes at least that much (RFC 792), and may quote more (RFC 1812). The quoted datagram's
// checksums aren't checked: the quote may be cut short.
bool ping_parse_error(const uint8_t *pkt, size_t len, uint16_t id, ping_error_t *e);

// What an error means: "host unreachable", "time to live exceeded", ...; for a code without a
// name, the type's with the code ("destination unreachable (code 8)").
void ping_error_text(uint8_t type, uint8_t code, char out[40]);

typedef struct {
    uint32_t sent;          // requests that went out
    uint32_t received;      // of those, the ones answered
    uint32_t icmp_errors;   // of those, the ones an ICMP error came back for (lost)
    uint32_t not_sent;      // requests that couldn't be sent
    uint32_t min_us, max_us;
    uint64_t sum_us;        // of the round-trip times of those answered
} ping_stats_t;

// Count a reply that came back after rtt_us microseconds.
void ping_stats_reply(ping_stats_t *s, uint32_t rtt_us);

// Microseconds as milliseconds, rounded to a tenth: 4250 -> "4.3".
void ping_ms_text(uint32_t us, char out[16]);

// The requests lost, as a rounded percentage of those sent: "25%". Never rounds to "0%" when one
// was lost or to "100%" when one came back: "<1%", ">99%". Empty if none were sent.
void ping_loss_text(const ping_stats_t *s, char out[8]);

// The summary line, without the host: "4 sent, 3 received, 25% loss; round trip min/avg/max
// 3.1/4.0/6.2 ms", with ", 1 ICMP error" and ", 1 not sent" after the loss when there were
// any. Returns the length (snprintf's: it may have been cut to fit).
int ping_summary(const ping_stats_t *s, char *out, size_t size);

#endif
