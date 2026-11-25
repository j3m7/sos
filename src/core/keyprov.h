// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The key provider: every key the firmware uses to protect stored data is derived here from
// one root secret (issue #64). Callers ask for a key by purpose and never see the root secret.
//
//   key = HKDF-SHA256(input key = root secret, no salt, info = the purpose's label, 32 bytes)
//
// - One fixed label per purpose, so a key made for one use can't stand in for another. A
//   caller can't supply any part of the label.
// - Where the root secret comes from is the backend's business: a flash sector on an open board
//   (core/rootflash.c), OTP memory on a secured one (later).
// - Derive at boot, then lock. All keys are derived during boot; keyprov_lock() ends that, and
//   from then on keyprov_derive() fails until reset. On an open board the lock is a flag. On a
//   secured board the backend also locks the OTP page until reset, so a later bug can't read
//   the root secret. For that reason no derivation may depend on something only known at run
//   time: a key that does (the SD card wrap key, which also depends on the owner secret) is
//   made by the caller in a second step from a base key derived here.
// - Callers wipe the keys they got when they're done with them.
// - Only the backend sees the root secret, for the length of one derivation.
//
// Pure C over wolfCrypt, so it's unit tested on the host.

#ifndef SOS_KEYPROV_H
#define SOS_KEYPROV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KEYPROV_KEY_LEN  32
#define KEYPROV_ROOT_LEN 32

// What a key is for. The labels are part of the stored formats: changing one changes the key,
// and what was sealed with the old key can no longer be opened. Add new purposes at the end.
enum {
    KEYPROV_CONFIG_SEAL,        // "SOS v1 config seal": seals the saved configuration
    KEYPROV_HOSTKEY_SEAL,       // "SOS v1 host key seal": seals the SSH host key
    KEYPROV_CONSOLE_PASSWORD,   // "SOS v1 console password": keys the password verifier
    KEYPROV_SD_WRAP_BASE,       // "SOS v1 sd card wrap base": with the owner secret, wraps cards
    KEYPROV_PURPOSES
};

typedef struct {
    // Copy the root secret into out. False if there's none that can be used.
    bool (*root)(void *ctx, uint8_t out[KEYPROV_ROOT_LEN]);
    // Make the root secret unreadable until reset, where the hardware can (may be NULL).
    void (*lock)(void *ctx);
    void *ctx;
} keyprov_backend_t;

// Start with this backend. Once per boot: later calls do nothing, so the backend can't be
// swapped and a locked provider stays locked.
void keyprov_init(const keyprov_backend_t *backend);

#ifdef KEYPROV_TEST
// Unit tests only (not compiled into the firmware): back to not started and unlocked.
void keyprov_test_reset(void);
#endif

// Derive the key for `purpose` into out. False, with `out` zeroed, if the provider is locked or
// not started, the purpose is unknown, or there's no usable root secret.
bool keyprov_derive(unsigned purpose, uint8_t out[KEYPROV_KEY_LEN]);

// No more derivations until reset.
void keyprov_lock(void);
bool keyprov_locked(void);

// The purpose's label (for messages and tests), or NULL.
const char *keyprov_label(unsigned purpose);

// Clear memory that held a key (not optimized away).
void keyprov_wipe(void *p, size_t len);

// HKDF-SHA256 (RFC 5869), exposed for tests. salt may be NULL (salt_len 0), info too.
bool keyprov_hkdf(const uint8_t *ikm, size_t ikm_len, const uint8_t *salt, size_t salt_len,
                  const uint8_t *info, size_t info_len, uint8_t *out, size_t out_len);

#endif
