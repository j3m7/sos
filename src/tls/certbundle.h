// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Certificate bundle parsing for /certs/roots.pem: a PEM bundle ("-----BEGIN CERTIFICATE-----"
// blocks, with any text between them, as in system CA files) or DER certificates back to back.
// Produces DER, so TLS loads every root the same way. Pure C (no wolfSSL), host-tested.

#ifndef SOS_CERTBUNDLE_H
#define SOS_CERTBUNDLE_H

#include <stddef.h>
#include <stdint.h>

#define CERTBUNDLE_MAX_CERTS 32

typedef struct {
    size_t off, len;            // the certificate's DER within the output buffer
} certbundle_cert_t;

// Parse `file` into DER certificates written to `out` (out_cap bytes; the file's length is
// always enough). Returns the number of certificates (1..max), or 0 with *why set if the file
// holds none, has a malformed or unterminated block, any PEM block other than a certificate
// (a private key, say), or more than `max` certificates.
size_t certbundle_parse(const uint8_t *file, size_t len, uint8_t *out, size_t out_cap,
                        certbundle_cert_t *certs, size_t max, const char **why);

#endif
