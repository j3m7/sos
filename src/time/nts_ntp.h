// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// NTS-protected NTPv4 packets (RFC 8915 section 5): building a client request and checking a
// server reply. Pure C, no I/O and no crypto library: the AEAD (AES-SIV-CMAC-256 on the device)
// is passed in, so the packet logic runs in the host tests.
//
// Request: NTP header (client mode, random transmit timestamp), then extension fields (RFC 7822:
// type, total length, body; lengths multiples of 4): Unique Identifier, NTS Cookie, Cookie
// Placeholders, and last the NTS Authenticator, whose AEAD tag covers everything before it.
// Reply: the same Unique Identifier and an Authenticator whose ciphertext holds new cookies.

#ifndef SOS_NTS_NTP_H
#define SOS_NTS_NTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NTS_NTP_PORT            123
#define NTS_NTP_HEADER_LEN      48
#define NTS_NTP_UID_LEN         32      // Unique Identifier (RFC 8915: at least 32)
#define NTS_NTP_NONCE_LEN       16      // what AES-SIV needs without extra padding
#define NTS_NTP_TAG_LEN         16      // AES-SIV synthetic IV, first in the ciphertext
#define NTS_NTP_MAX_COOKIES     8
#define NTS_NTP_COOKIE_MAX      256
#define NTS_NTP_PACKET_MAX      1280    // request or reply; 8 x 100-byte cookies is ~1 KB

// Extension field types.
enum {
    NTS_EF_UNIQUE_ID = 0x0104,
    NTS_EF_COOKIE = 0x0204,
    NTS_EF_COOKIE_PLACEHOLDER = 0x0304,
    NTS_EF_AUTHENTICATOR = 0x0404,
};

// AEAD with the key bound in ctx. seal writes tag (16 bytes) then ciphertext (ptlen bytes) to
// out; open checks the tag at the start of ct and writes ctlen - 16 bytes of plaintext. Both
// return true on success.
typedef struct {
    bool (*seal)(void *ctx, const uint8_t *ad, size_t adlen, const uint8_t *nonce,
                 size_t noncelen, const uint8_t *pt, size_t ptlen, uint8_t *out);
    bool (*open)(void *ctx, const uint8_t *ad, size_t adlen, const uint8_t *nonce,
                 size_t noncelen, const uint8_t *ct, size_t ctlen, uint8_t *pt);
    void *ctx;
} nts_aead_t;

// Build a request using cookie, asking for `placeholders` more cookies (0-7). xmt is the
// transmit timestamp to send (random: the real send time is kept locally), uid and nonce are
// fresh random values. Returns the packet length, or 0 if it doesn't fit or sealing fails.
size_t nts_ntp_build_request(uint8_t *buf, size_t buflen, const uint8_t xmt[8],
                             const uint8_t uid[NTS_NTP_UID_LEN], const uint8_t *cookie,
                             size_t cookie_len, unsigned placeholders,
                             const uint8_t nonce[NTS_NTP_NONCE_LEN], const nts_aead_t *c2s);

typedef struct {
    uint8_t leap, version, mode, stratum;
    int8_t poll, precision;
    uint32_t root_delay, root_dispersion;   // NTP short format (16.16 seconds)
    uint8_t refid[4];
    uint64_t reference, origin, receive, transmit;  // NTP timestamps (32.32 since 1900)
    unsigned cookie_count;                  // new cookies from the encrypted part
    uint16_t cookie_len[NTS_NTP_MAX_COOKIES];
    uint8_t cookies[NTS_NTP_MAX_COOKIES][NTS_NTP_COOKIE_MAX];
} nts_ntp_reply_t;

typedef enum {
    NTS_NTP_OK = 0,         // authenticated reply to our request
    NTS_NTP_NAK = 1,        // Kiss-o'-Death "NTSN" for our request: run NTS-KE again
    NTS_NTP_INVALID = -1,   // malformed, not ours, or not authentic; see the reason string
} nts_ntp_status_t;

// Check a reply against the request that was sent (its transmit timestamp and Unique
// Identifier) and authenticate it with the server-to-client AEAD. Only an NTS_NTP_OK result's
// fields can be trusted; the caller still checks leap, stratum and the timestamps make sense.
// `plain` is working space the encrypted part of the reply is opened into, `plain_size` bytes
// (NTS_NTP_PACKET_MAX holds any reply; a reply whose encrypted part is longer is invalid). It
// must not overlap `pkt`. What it holds afterwards is of no use to the caller.
nts_ntp_status_t nts_ntp_parse_reply(const uint8_t *pkt, size_t len, const uint8_t xmt[8],
                                     const uint8_t uid[NTS_NTP_UID_LEN], const nts_aead_t *s2c,
                                     uint8_t *plain, size_t plain_size, nts_ntp_reply_t *out,
                                     const char **reason);

// NTP timestamp (era 0 until 2036, then era 1) <-> microseconds since 1970.
int64_t nts_ntp_to_unix_us(uint64_t ntp);
uint64_t nts_ntp_from_unix_us(int64_t us);

// Clock offset (server minus local) and round-trip delay from the four timestamps, all in
// microseconds since 1970: t1 request sent, t2 server received, t3 server sent, t4 reply received.
void nts_ntp_offset(int64_t t1, int64_t t2, int64_t t3, int64_t t4, int64_t *offset,
                    int64_t *delay);

#endif
