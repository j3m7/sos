// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "files/cardkey.h"

#include <string.h>

#include "core/keyprov.h"
#include "core/log.h"
#include "core/settings.h"
#include "files/sdcard.h"
#include "files/sdcrypt.h"

_Static_assert(CARDKEY_LEN == SDCRYPT_DEVKEY_LEN && CARDKEY_LEN == KEYPROV_KEY_LEN &&
               CARDKEY_LEN == CONFIG_OWNER_SECRET_LEN, "key sizes");

// The wrap base key: in RAM while the board runs (the owner secret it's combined with can
// change at run time, after the key provider has locked). Nothing copies it elsewhere.
static uint8_t base[KEYPROV_KEY_LEN];
static bool have_base;

void cardkey_init(void) {
    settings_on_owner_gone(sdcard_owner_gone);     // `config reset`: close an encrypted card
    have_base = keyprov_derive(KEYPROV_SD_WRAP_BASE, base);
    if (!have_base)
        log_msg(SOS_LOG_DAEMON | SOS_LOG_ERR,
                "sd: no key for encrypted cards (see `rootsecret`): they can't be "
                "opened or made");
}

bool cardkey_possible(void) {
    return have_base;
}

bool cardkey_get(uint8_t out[CARDKEY_LEN]) {
    memset(out, 0, CARDKEY_LEN);
    return have_base && settings_has_owner_secret() &&
           sdcrypt_wrap_key(out, base, settings()->owner_secret);
}

bool cardkey_ensure(void) {
    if (!have_base)
        return false;
    // Saving makes the owner secret if there's none (core/settings.c).
    return settings_has_owner_secret() || (settings_save() && settings_has_owner_secret());
}
