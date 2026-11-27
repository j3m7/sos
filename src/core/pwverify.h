// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The console password's verifier (issue #64). The password itself is never stored. What the
// sealed configuration holds is a salt, an iteration count and
//
//   verifier = HMAC-SHA256(key = console-password key,
//                          message = PBKDF2-HMAC-SHA256(password, salt, iterations, 32 bytes))
//
// - The console-password key comes from the board's root secret (KEYPROV_CONSOLE_PASSWORD). So
//   a copy of the flash isn't enough to try passwords against: that also takes the root secret,
//   and whoever has that can open the configuration anyway.
// - PBKDF2 makes each try on the board itself slow (a few hundred milliseconds). It isn't
//   memory-hard: the board hasn't the RAM, and the keying above is what stops offline guessing.
// - The salt and the iteration count are stored with the verifier, so the cost can be raised
//   later without invalidating a password.
//
// The slow part runs in slices (pwv_step), so the main loop keeps turning while a password is
// checked. Pure C over wolfCrypt's HMAC; unit tested on the host.

#ifndef SOS_PWVERIFY_H
#define SOS_PWVERIFY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/hmac.h>

#define PWV_SALT_LEN       16
#define PWV_LEN            32           // the verifier, and the keys
#define PWV_ITERATIONS_MIN 1000         // a stored count below this isn't accepted
#define PWV_ITERATIONS_MAX 1000000

typedef struct {
    Hmac hmac;                  // keyed with the password
    uint8_t u[PWV_LEN], t[PWV_LEN];
    uint32_t left;              // iterations still to do
    bool running, failed;
} pwv_job_t;

// Begin deriving from a password. False if the iteration count is out of range or the HMAC
// can't be set up. The password isn't kept: the caller may wipe it once this returns.
bool pwv_start(pwv_job_t *job, const char *password, size_t len, const uint8_t salt[PWV_SALT_LEN],
               uint32_t iterations);

// Do up to `max_iterations` more. True when the derivation is complete (or has failed).
bool pwv_step(pwv_job_t *job, uint32_t max_iterations);

// Finish: write the verifier for `key` to out and wipe the job. False (out zeroed) if the
// derivation didn't complete or failed.
bool pwv_finish(pwv_job_t *job, const uint8_t key[PWV_LEN], uint8_t out[PWV_LEN]);

// Drop a job without finishing it (wipes it).
void pwv_abort(pwv_job_t *job);

// Compare two verifiers in constant time.
bool pwv_equal(const uint8_t a[PWV_LEN], const uint8_t b[PWV_LEN]);

// All in one, for setting a password (blocks for the whole derivation). `job` is working space
// from the caller (an Hmac is large for the stack); it must not be one that is in use, and
// holds nothing afterwards.
bool pwv_compute(pwv_job_t *job, const char *password, size_t len,
                 const uint8_t salt[PWV_SALT_LEN], uint32_t iterations,
                 const uint8_t key[PWV_LEN], uint8_t out[PWV_LEN]);

#endif
