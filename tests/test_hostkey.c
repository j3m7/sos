// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/ssh/hostkey.c: the host key over the life of a board and its owners. The
// record and seal code are the real ones (hostkey_record.c, core/seal.c); the flash page, the
// key provider, the settings, key generation and the log are stand-ins here.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/config.h"
#include "core/keyprov.h"
#include "core/keysector.h"
#include "core/log.h"
#include "core/random.h"
#include "core/seal.h"
#include "core/consoleonly.h"
#include "core/settings.h"
#include "ssh/hostkey.h"
#include "ssh/hostkey_record.h"
#include "test.h"

// --- stand-ins ----------------------------------------------------------------------------------------

static uint8_t flash_page[HOSTREC_PAGE];        // the key sector's host key page
static int page_writes;
static bool write_fails;
const uint8_t *keysector_page(unsigned page) { (void)page; return flash_page; }
bool keysector_write(unsigned page, const uint8_t *data) {
    (void)page;
    if (write_fails)
        return false;
    page_writes++;
    if (data)
        memcpy(flash_page, data, HOSTREC_PAGE);
    else
        memset(flash_page, 0xff, HOSTREC_PAGE);
    return true;
}

static bool have_root = true;
static uint8_t root_base[KEYPROV_KEY_LEN];
static bool provider_locked;
bool keyprov_derive(unsigned purpose, uint8_t out[KEYPROV_KEY_LEN]) {
    CHECK_EQ(purpose, KEYPROV_HOSTKEY_SEAL);
    CHECK(!provider_locked);                    // only ever asked at boot
    if (!have_root) {
        memset(out, 0, KEYPROV_KEY_LEN);
        return false;
    }
    memcpy(out, root_base, KEYPROV_KEY_LEN);
    return true;
}

static sos_config_t cfg;
sos_config_t *settings(void) { return &cfg; }
bool settings_has_owner_secret(void) {
    for (unsigned i = 0; i < sizeof(cfg.owner_secret); i++)
        if (cfg.owner_secret[i])
            return true;
    return false;
}

static bool is_console_only;
bool consoleonly(void) { return is_console_only; }

static unsigned rng;
bool random_bytes(void *ctx, uint8_t *out, size_t len) {
    (void)ctx;
    for (size_t i = 0; i < len; i++) {
        rng = rng * 1103515245u + 12345u;
        out[i] = (uint8_t)(rng >> 16);
    }
    return true;
}

static int keys_made;
// Stands in for wolfSSH's key generation, which hostkey.c declares itself.
int wolfSSH_MakeEd25519Key(uint8_t *out, uint32_t out_size, uint32_t size);
int wolfSSH_MakeEd25519Key(uint8_t *out, uint32_t out_size, uint32_t size) {
    (void)size;
    CHECK(out_size >= 82);
    keys_made++;
    for (unsigned i = 0; i < 82; i++)
        out[i] = (uint8_t)((unsigned)keys_made * 16 + i);  // each key made is different
    return 82;
}

static char last_log[512];
void log_msg(int level, const char *fmt, ...) {
    (void)level;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    va_end(ap);
}

// --- helpers ------------------------------------------------------------------------------------------

static void new_owner(uint8_t seed) {
    for (unsigned i = 0; i < sizeof(cfg.owner_secret); i++)
        cfg.owner_secret[i] = (uint8_t)(seed + i);
}
static void no_owner(void) { memset(cfg.owner_secret, 0, sizeof(cfg.owner_secret)); }

// A boot: the base key is derived, the provider locks, and whatever RAM held is gone. The
// flash page and the configuration are what they were.
static void boot(void) {
    provider_locked = false;
    hostkey_owner_gone();                       // stands for RAM being cleared by the reset
    hostkey_init();
    provider_locked = true;
}

static void factory(void) {
    memset(flash_page, 0xff, sizeof(flash_page));
    memset(&cfg, 0, sizeof(cfg));
    for (unsigned i = 0; i < sizeof(root_base); i++)
        root_base[i] = (uint8_t)(0xc0 + i);
    have_root = true;
    is_console_only = write_fails = false;
    page_writes = keys_made = 0;
    rng = 99;
    boot();
}

static bool get(uint8_t key[82]) {
    const uint8_t *der;
    uint32_t len = 0;
    bool ok = hostkey_get(&der, &len);
    if (ok) {
        CHECK_EQ(len, 82);
        memcpy(key, der, 82);
    } else {
        CHECK_EQ(len, 0);
    }
    return ok;
}

// --- tests --------------------------------------------------------------------------------------------

// A board that isn't set up has no host key, however often it is asked, and nothing is made
// or written. Once it is set up it gets one, without a reboot, and keeps it.
TEST(no_host_key_until_the_board_has_an_owner) {
    uint8_t k1[82], k2[82];
    factory();
    for (int i = 0; i < 5; i++)
        CHECK(!get(k1));
    CHECK_EQ(keys_made, 0);
    CHECK_EQ(page_writes, 0);
    new_owner(1);                               // set up on the console
    CHECK(get(k1));
    CHECK_EQ(keys_made, 1);
    CHECK_EQ(hostrec_kind(flash_page), HOSTREC_SEALED);
    CHECK(get(k2));                             // asked again: the same key, nothing redone
    CHECK(!memcmp(k1, k2, 82));
    CHECK_EQ(keys_made, 1);
    CHECK_EQ(page_writes, 1);
    boot();                                     // and after a reboot
    CHECK(get(k2));
    CHECK(!memcmp(k1, k2, 82));
    CHECK_EQ(keys_made, 1);
}

// The gap this closes: `config reset` and a new setup in one session, with no reboot. The
// host key in RAM was the old owner's. It must be gone the moment the owner secret is, and
// the next owner must get a different one.
TEST(config_reset_takes_the_host_key_with_the_owner) {
    uint8_t old_key[82], new_key[82];
    factory();
    new_owner(1);
    CHECK(get(old_key));
    // `config reset`: the owner secret is destroyed, sshd is stopped, the key is forgotten.
    no_owner();
    hostkey_owner_gone();
    CHECK(!get(new_key));                       // no owner: no host key, not the old one
    CHECK(!get(new_key));
    CHECK_EQ(keys_made, 1);
    // `setup`, same session.
    new_owner(2);
    CHECK(get(new_key));
    CHECK_EQ(keys_made, 2);
    CHECK(memcmp(new_key, old_key, 82) != 0);   // a new identity
    // The stored record is the new owner's too: after a reboot it is still the new key.
    boot();
    CHECK(get(old_key));
    CHECK(!memcmp(new_key, old_key, 82));
    CHECK_EQ(keys_made, 2);
}

// WITHOUT that step the old key would still be handed out: this is what the code did before,
// kept as a test of the test (hostkey_get caches the key it has).
TEST(without_owner_gone_the_old_key_would_stay) {
    uint8_t old_key[82], k[82];
    factory();
    new_owner(1);
    CHECK(get(old_key));
    no_owner();
    new_owner(2);                               // reset and setup, the key not forgotten
    CHECK(get(k));
    CHECK(!memcmp(k, old_key, 82));             // the old owner's key: the defect
    hostkey_owner_gone();
    CHECK(get(k));
    CHECK(memcmp(k, old_key, 82) != 0);
}

// The same over a reboot: an erase over USB, then the board set up again.
TEST(a_wipe_over_usb_then_a_new_owner) {
    uint8_t old_key[82], new_key[82];
    factory();
    new_owner(1);
    CHECK(get(old_key));
    no_owner();                                 // the configuration sectors erased
    boot();
    CHECK(!get(new_key));
    CHECK_EQ(hostrec_kind(flash_page), HOSTREC_SEALED);     // the old record is left alone
    CHECK_EQ(page_writes, 1);
    new_owner(2);
    CHECK(get(new_key));
    CHECK(memcmp(new_key, old_key, 82) != 0);
    // The same owner coming back (a restored configuration) would still not get the old key
    // back: the record was replaced.
    new_owner(1);
    boot();
    CHECK(get(old_key));
    CHECK(memcmp(old_key, new_key, 82) != 0);
}

// No root secret: no host key, and asking again doesn't retry until a reboot.
TEST(no_root_secret) {
    uint8_t k[82];
    factory();
    have_root = false;
    new_owner(1);
    boot();
    CHECK(!get(k));
    CHECK(!get(k));
    CHECK_EQ(keys_made, 0);
    CHECK_EQ(page_writes, 0);
}

// A key that can't be stored isn't used: a host key that would change at the next boot is
// worse than none. Final until a reboot.
TEST(a_key_that_cannot_be_stored_is_not_used) {
    uint8_t k[82];
    factory();
    new_owner(1);
    write_fails = true;
    CHECK(!get(k));
    write_fails = false;
    CHECK(!get(k));                             // not retried in this boot
    CHECK_EQ(keys_made, 1);
    boot();
    CHECK(get(k));
}

// A record from firmware before this (sealed under the base key alone): an open board takes
// it over, same key; a console-only board makes a new one.
TEST(a_version_2_record) {
    uint8_t made[82], k[82], v2[HOSTREC_PAGE];
    factory();
    new_owner(1);
    CHECK(get(made));
    // Rebuild that key as a version 2 record, as older firmware stored it.
    seal_ctx_t old = { .random = random_bytes };
    memcpy(old.key, root_base, sizeof(root_base));
    old.have_key = true;
    sealer_t old_sealer = seal_sealer(&old);
    memset(v2, 0xff, sizeof(v2));
    memcpy(v2, "SOSK", 4);
    v2[4] = 2; v2[5] = v2[6] = v2[7] = 0;
    v2[8] = 82; v2[9] = v2[10] = v2[11] = 0;
    CHECK(old_sealer.seal(old_sealer.ctx, v2, 12, made, 82, v2 + 12));
    memcpy(flash_page, v2, sizeof(v2));
    CHECK_EQ(hostrec_kind(flash_page), HOSTREC_SEALED_OLD);

    boot();                                     // an open board
    CHECK(get(k));
    CHECK(!memcmp(k, made, 82));                // the same key
    CHECK_EQ(hostrec_kind(flash_page), HOSTREC_SEALED);     // now stored for the owner
    CHECK_EQ(keys_made, 1);

    memcpy(flash_page, v2, sizeof(v2));
    is_console_only = true;
    boot();
    CHECK(get(k));
    CHECK(memcmp(k, made, 82) != 0);            // not carried over: a new key
    CHECK_EQ(keys_made, 2);
    CHECK_EQ(hostrec_kind(flash_page), HOSTREC_SEALED);
}

TEST_MAIN(
    RUN(no_host_key_until_the_board_has_an_owner);
    RUN(config_reset_takes_the_host_key_with_the_owner);
    RUN(without_owner_gone_the_old_key_would_stay);
    RUN(a_wipe_over_usb_then_a_new_owner);
    RUN(no_root_secret);
    RUN(a_key_that_cannot_be_stored_is_not_used);
    RUN(a_version_2_record);
)
