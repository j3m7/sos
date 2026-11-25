// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The interface the record formats (core/config.c, ssh/hostkey_record.c) seal and open their
// contents through. It has no cryptography in it, so those formats stay pure C: the device
// plugs in AES-256-GCM (core/seal.h), the unit tests a fake, and the host provisioning tool
// nothing at all.
//
// A sealed blob is SEALER_OVERHEAD bytes longer than what it holds:
//   nonce (12) | ciphertext (same length as the plain data) | tag (16)
// The associated data (a record's header) isn't stored in the blob, but opening fails unless
// it's the same as when the blob was sealed.
//
// The buffers must not overlap: not the data and the blob, nor either with the associated data.
//
// What sealing does not give: freshness. An older blob sealed by the same board still opens, so
// someone who can rewrite the flash can put back an earlier sealed record (an old password, a
// trusted key that was since removed). Stopping that needs a counter the flash can't roll back
// (OTP); issue #64 accepts it for now.

#ifndef SOS_SEALER_H
#define SOS_SEALER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SEALER_NONCE_LEN 12
#define SEALER_TAG_LEN   16
#define SEALER_OVERHEAD  (SEALER_NONCE_LEN + SEALER_TAG_LEN)

typedef struct {
    // Seal len bytes of plain into out (len + SEALER_OVERHEAD bytes), with a fresh nonce.
    // False if it can't (no key, no random numbers); out is then zeroed.
    bool (*seal)(void *ctx, const uint8_t *aad, size_t aad_len, const uint8_t *plain, size_t len,
                 uint8_t *out);
    // Open a blob of sealed_len bytes into plain (sealed_len - SEALER_OVERHEAD bytes). False,
    // with plain zeroed, if it doesn't authenticate: another key, other associated data, or
    // any change to the blob.
    bool (*open)(void *ctx, const uint8_t *aad, size_t aad_len, const uint8_t *sealed,
                 size_t sealed_len, uint8_t *plain);
    void *ctx;
} sealer_t;

#endif
