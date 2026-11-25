// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The key provider (see keyprov.h). Pure C over wolfCrypt.

#include "core/keyprov.h"

#include <string.h>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/hmac.h>

// Fixed and complete: nothing is appended to a label, so no two purposes can produce the same
// HKDF info. None is a prefix of another either.
static const char *const labels[KEYPROV_PURPOSES] = {
    [KEYPROV_CONFIG_SEAL]      = "SOS v1 config seal",
    [KEYPROV_HOSTKEY_SEAL]     = "SOS v1 host key seal",
    [KEYPROV_CONSOLE_PASSWORD] = "SOS v1 console password",
    [KEYPROV_SD_WRAP_BASE]     = "SOS v1 sd card wrap base",
};

static const keyprov_backend_t *backend;
static bool started, locked;

void keyprov_wipe(void *p, size_t len) {
    volatile uint8_t *v = p;
    while (len--)
        *v++ = 0;
}

bool keyprov_hkdf(const uint8_t *ikm, size_t ikm_len, const uint8_t *salt, size_t salt_len,
                  const uint8_t *info, size_t info_len, uint8_t *out, size_t out_len) {
    return wc_HKDF(WC_SHA256, ikm, (word32)ikm_len, salt, (word32)salt_len, info, (word32)info_len,
                   out, (word32)out_len) == 0;
}

void keyprov_init(const keyprov_backend_t *b) {
    if (started)                // once per boot: a second call can't swap the backend or unlock
        return;
    started = true;
    backend = b;
}

#ifdef KEYPROV_TEST
void keyprov_test_reset(void) {
    started = locked = false;
    backend = NULL;
}
#endif

const char *keyprov_label(unsigned purpose) {
    return purpose < KEYPROV_PURPOSES ? labels[purpose] : NULL;
}

bool keyprov_derive(unsigned purpose, uint8_t out[KEYPROV_KEY_LEN]) {
    memset(out, 0, KEYPROV_KEY_LEN);
    const char *label = keyprov_label(purpose);
    if (!backend || !backend->root || locked || !label)
        return false;
    uint8_t root[KEYPROV_ROOT_LEN];
    bool ok = backend->root(backend->ctx, root) &&
              keyprov_hkdf(root, sizeof(root), NULL, 0, (const uint8_t *)label, strlen(label), out,
                           KEYPROV_KEY_LEN);
    keyprov_wipe(root, sizeof(root));
    if (!ok)
        keyprov_wipe(out, KEYPROV_KEY_LEN);
    return ok;
}

void keyprov_lock(void) {
    if (locked)
        return;
    locked = true;
    if (backend && backend->lock)
        backend->lock(backend->ctx);
}

bool keyprov_locked(void) {
    return locked;
}
