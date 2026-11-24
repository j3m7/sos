// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// NTS Key Establishment (RFC 8915 section 4) message encoding: the client request and parsing
// of the server's response records. Pure C, no I/O: the TLS connection is src/time/nts.c's job.
//
// A record is: 1 bit critical, 15 bits type, 16 bits body length, then the body (big-endian).

#ifndef SOS_NTS_KE_H
#define SOS_NTS_KE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NTS_KE_PORT             4460
#define NTS_KE_ALPN             "ntske/1"
#define NTS_PROTOCOL_NTPV4      0
#define NTS_AEAD_AES_SIV_CMAC_256 15
#define NTS_KEY_LEN             32      // per direction, for AEAD_AES_SIV_CMAC_256

#define NTS_KE_MAX_COOKIES      8
#define NTS_KE_COOKIE_MAX       256     // bytes; RFC 8915 sets no limit, servers use ~100
#define NTS_KE_SERVER_MAX       255
#define NTS_KE_RESPONSE_MAX     4096    // whole response; with 8 cookies it's well under 2 KB

// Record types.
enum {
    NTS_KE_END = 0,
    NTS_KE_NEXT_PROTOCOL = 1,
    NTS_KE_ERROR = 2,
    NTS_KE_WARNING = 3,
    NTS_KE_AEAD = 4,
    NTS_KE_COOKIE = 5,
    NTS_KE_NTPV4_SERVER = 6,
    NTS_KE_NTPV4_PORT = 7,
};

typedef struct {
    uint16_t aead;                                  // negotiated AEAD algorithm
    unsigned cookie_count;
    uint16_t cookie_len[NTS_KE_MAX_COOKIES];
    uint8_t cookies[NTS_KE_MAX_COOKIES][NTS_KE_COOKIE_MAX];
    char server[NTS_KE_SERVER_MAX + 1];             // NTP server to use; "" = the NTS-KE host
    uint16_t port;                                  // NTP port; 0 = 123
    int error_code;                                 // from an Error record, or -1
    int warning_code;                               // from a Warning record, or -1
} nts_ke_result_t;

typedef enum {
    NTS_KE_OK = 0,          // complete, valid response
    NTS_KE_NEED_MORE = 1,   // no End of Message yet
    NTS_KE_INVALID = -1,    // malformed or unacceptable; see the reason string
} nts_ke_status_t;

// The request: Next Protocol (NTPv4), AEAD (AES-SIV-CMAC-256), End of Message. Returns its
// length, or 0 if buflen is too small (16 bytes are enough).
size_t nts_ke_build_request(uint8_t *buf, size_t buflen);

// Parse the response received so far (buf holds everything from the first byte). Returns
// NTS_KE_NEED_MORE until End of Message arrives. On NTS_KE_INVALID, *reason says why.
nts_ke_status_t nts_ke_parse(const uint8_t *buf, size_t len, nts_ke_result_t *res,
                             const char **reason);

// TLS exporter context for a key (RFC 8915 section 5.1): protocol id, AEAD id, then 0 for the
// client-to-server key or 1 for server-to-client.
void nts_ke_exporter_context(uint16_t aead, bool s2c, uint8_t ctx[5]);
#define NTS_KE_EXPORTER_LABEL "EXPORTER-network-time-security"

#endif
