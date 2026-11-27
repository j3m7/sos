// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The parts of tools/host/sdcrypt.c that tests/test_sdcrypt.c calls (built with
// SDCRYPT_TOOL_NO_MAIN), to cross-check src/files/sdcrypt.c against OpenSSL.

#ifndef SOS_SDCRYPT_TOOL_H
#define SOS_SDCRYPT_TOOL_H

#include <stdint.h>

// XTS-AES-128 of `count` sectors from logical sector `lsector`. Returns 0 on success.
int sdcrypt_tool_xts(int encrypt, const uint8_t key[32], uint64_t lsector,
                     const uint8_t *in, uint8_t *out, unsigned count);
// Parses a header sector: 0 and the data offset, or -1 if it isn't one the tool knows.
int sdcrypt_tool_header(const uint8_t sector[512], uint32_t *offset);

#endif
