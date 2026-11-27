// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The console password's verifier (see pwverify.h). Pure C over wolfCrypt's HMAC.

#include "core/pwverify.h"

#include <string.h>

static void wipe(void *p, size_t len) {
    volatile uint8_t *v = p;
    while (len--)
        *v++ = 0;
}

void pwv_abort(pwv_job_t *job) {
    if (job->running)
        wc_HmacFree(&job->hmac);
    wipe(job, sizeof(*job));
}

// PBKDF2 (RFC 8018) with HMAC-SHA256 and a 32-byte output is a single block:
//   U1 = HMAC(P, salt || 00 00 00 01),  Ui = HMAC(P, U(i-1)),  T = U1 ^ U2 ^ ... ^ Uc
bool pwv_start(pwv_job_t *job, const char *password, size_t len, const uint8_t salt[PWV_SALT_LEN],
               uint32_t iterations) {
    static const uint8_t block_one[4] = { 0, 0, 0, 1 };
    memset(job, 0, sizeof(*job));
    if (iterations < PWV_ITERATIONS_MIN || iterations > PWV_ITERATIONS_MAX)
        return false;
    if (wc_HmacInit(&job->hmac, NULL, INVALID_DEVID) != 0)
        return false;
    job->running = true;
    if (wc_HmacSetKey(&job->hmac, WC_SHA256, (const byte *)password, (word32)len) != 0 ||
        wc_HmacUpdate(&job->hmac, salt, PWV_SALT_LEN) != 0 ||
        wc_HmacUpdate(&job->hmac, block_one, sizeof(block_one)) != 0 ||
        wc_HmacFinal(&job->hmac, job->u) != 0) {
        pwv_abort(job);
        return false;
    }
    memcpy(job->t, job->u, PWV_LEN);
    job->left = iterations - 1;
    return true;
}

bool pwv_step(pwv_job_t *job, uint32_t max_iterations) {
    if (!job->running || job->failed)
        return true;
    while (job->left && max_iterations--) {
        // wolfCrypt's HMAC keeps its key after wc_HmacFinal, ready for the next message.
        if (wc_HmacUpdate(&job->hmac, job->u, PWV_LEN) != 0 ||
            wc_HmacFinal(&job->hmac, job->u) != 0) {
            job->failed = true;
            return true;
        }
        for (unsigned i = 0; i < PWV_LEN; i++)
            job->t[i] ^= job->u[i];
        job->left--;
    }
    return job->left == 0;
}

bool pwv_finish(pwv_job_t *job, const uint8_t key[PWV_LEN], uint8_t out[PWV_LEN]) {
    bool ok = job->running && !job->failed && job->left == 0;
    Hmac keyed;
    memset(out, 0, PWV_LEN);
    if (ok) {
        bool init = wc_HmacInit(&keyed, NULL, INVALID_DEVID) == 0;
        ok = init && wc_HmacSetKey(&keyed, WC_SHA256, key, PWV_LEN) == 0 &&
             wc_HmacUpdate(&keyed, job->t, PWV_LEN) == 0 && wc_HmacFinal(&keyed, out) == 0;
        if (init)
            wc_HmacFree(&keyed);
        wipe(&keyed, sizeof(keyed));
        if (!ok)
            wipe(out, PWV_LEN);
    }
    pwv_abort(job);
    return ok;
}

bool pwv_equal(const uint8_t a[PWV_LEN], const uint8_t b[PWV_LEN]) {
    volatile uint8_t diff = 0;
    for (unsigned i = 0; i < PWV_LEN; i++)
        diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

bool pwv_compute(pwv_job_t *job, const char *password, size_t len,
                 const uint8_t salt[PWV_SALT_LEN], uint32_t iterations,
                 const uint8_t key[PWV_LEN], uint8_t out[PWV_LEN]) {
    memset(out, 0, PWV_LEN);
    if (!pwv_start(job, password, len, salt, iterations))
        return false;
    pwv_step(job, iterations);
    return pwv_finish(job, key, out);
}
