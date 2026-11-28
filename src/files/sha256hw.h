// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The RP2350's SHA-256 block, for sha256sum and fetch (docs/design/archives.md §4.3), through
// the SDK's pico_sha256. One block per chip: whoever gets it keeps it for a whole hash (the
// block holds the state between updates, so a file's hash starts and finishes on it), and
// anyone else hashes with wolfCrypt as before. TLS, SSH and Lua always use wolfCrypt.
//
//   if (sha256hw_begin()) { sha256hw_update(...); ...; sha256hw_finish(digest); }
//   else                  { wolfCrypt as before }
//
// Every end of a hash begun here must call sha256hw_finish() or sha256hw_abort(), or the
// block stays taken and everyone falls back to wolfCrypt (correct, only slower).
//
// Core 0 only (the main loop's jobs). The lock is the bootrom's SHA-256 boot lock, a hardware
// lock both cores and the bootrom respect, taken without waiting.
//
// The first begin hashes two FIPS 180-2 vectors on the block. A wrong answer is logged once and
// the block is never used again, so a misconfiguration can't give wrong hashes silently.

#ifndef SOS_SHA256HW_H
#define SOS_SHA256HW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SHA256HW_DIGEST 32

// Take the block and start a hash on it. False when it's taken (another shell's hash, or the
// bootrom), or failed its self-check: hash with wolfCrypt instead.
bool sha256hw_begin(void);

// Add bytes. Blocking: data may change once it returns. Word-aligned data whose earlier updates
// were multiples of 4 bytes goes in a word at a time; anything else a byte at a time (slower).
void sha256hw_update(const uint8_t *data, size_t len);

// The digest into out, and the block released.
void sha256hw_finish(uint8_t out[SHA256HW_DIGEST]);

// The hash abandoned and the block released (an error, Ctrl-C, the session ended).
void sha256hw_abort(void);

#endif
