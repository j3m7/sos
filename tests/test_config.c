// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/config.c: A/B record storage, validation, key parsing and the key list.

#include <stdint.h>

#include "core/config.h"
#include "test.h"

// Public keys generated with ssh-keygen for these tests (no private parts anywhere).
#define ED25519_LINE "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIJK3YOM3IDvlmN1xnYQm8HBTPfNlEa0FyWLOgTLqQt+x alice@laptop"
#define ECDSA_LINE   "ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBBJYwefdIfYjG5Vfsih2eq16zeCC4jakEGQKVYqHlpaPeH1gxQ8yWgi4r8BeoggJql5W6m/HevYH2MLYXoo94v8= bob@desk"
#define RSA_LINE     "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQDV rsa@x"

// --- fake flash: two sectors, erase sets 0xff ---------------------------------------------------

typedef struct {
    uint8_t slot[2][CONFIG_SLOT_SIZE];
    int writes;
    bool fail;              // write() reports failure without touching flash
    size_t tear_after;      // if non-zero, the next write stops after this many bytes (power cut)
} fake_flash_t;

static const uint8_t *flash_read(void *ctx, unsigned slot) {
    return ((fake_flash_t *)ctx)->slot[slot];
}

static bool flash_write(void *ctx, unsigned slot, const void *data, size_t len) {
    fake_flash_t *f = ctx;
    CHECK(slot < 2 && len <= CONFIG_SLOT_SIZE);
    if (f->fail)
        return false;
    f->writes++;
    memset(f->slot[slot], 0xff, CONFIG_SLOT_SIZE);
    if (f->tear_after) {
        memcpy(f->slot[slot], data, f->tear_after < len ? f->tear_after : len);
        f->tear_after = 0;
        return true;                            // the writer never learns it was cut short
    }
    memcpy(f->slot[slot], data, len);
    return true;
}

static fake_flash_t flash;
static config_store_t store = { flash_read, flash_write, &flash, NULL, false };

static void blank_flash(void) {
    memset(&flash, 0, sizeof(flash));
    memset(flash.slot, 0xff, sizeof(flash.slot));
}

// Recompute the CRC of the record in `slot` after a test patched it. Record layout: header
// magic(4) version(2) length(2) seq(4) crc(4), payload at offset 16; the CRC covers the header up
// to crc and then `length` bytes of payload (the same as config.c).
static void fix_crc(unsigned slot) {
    static uint8_t crc_input[12 + CONFIG_SLOT_SIZE];
    uint16_t length;
    memcpy(&length, flash.slot[slot] + 6, 2);
    CHECK(16 + (size_t)length <= CONFIG_SLOT_SIZE);
    memcpy(crc_input, flash.slot[slot], 12);
    memcpy(crc_input + 12, flash.slot[slot] + 16, length);
    uint32_t crc = config_crc32(crc_input, 12 + (size_t)length);
    memcpy(flash.slot[slot] + 12, &crc, 4);
}

static void set_length(unsigned slot, uint16_t length) {
    memcpy(flash.slot[slot] + 6, &length, 2);
    fix_crc(slot);
}

// Payload length of records saved by the first release (before ntp_sync_hours existed).
#define V1_LENGTH 1540
// ...and by the second (before certs_hash existed).
#define V2_LENGTH 1606
// ...and by the third (before wifi_powersave existed).
#define V3_LENGTH 1638
// ...and by the fourth (before wifi_country existed).
#define V4_LENGTH 1640

static void sample(sos_config_t *c, const char *ssid) {
    config_defaults(c);
    strcpy(c->wifi_ssid, ssid);
    strcpy(c->wifi_password, "correct horse");
    strcpy(c->hostname, "pico");
    strcpy(c->ssh_user, "julio");
    config_key_t k;
    CHECK(config_key_parse(ED25519_LINE, &k) == NULL);
    CHECK(config_key_add(c, &k) == NULL);
}

// --- storage ------------------------------------------------------------------------------------

TEST(blank_flash_loads_defaults) {
    sos_config_t c;
    blank_flash();
    memset(&c, 0x55, sizeof(c));
    CHECK(!config_load(&store, &c, NULL));
    CHECK(!strcmp(c.hostname, "sos"));
    CHECK(!strcmp(c.ssh_user, "admin"));
    CHECK_EQ(c.wifi_ssid[0], 0);
    CHECK_EQ(c.key_count, 0);
}

TEST(save_and_load_round_trip) {
    sos_config_t a, b;
    uint32_t seq = 0;
    blank_flash();
    sample(&a, "home");
    CHECK(config_save(&store, &a));
    CHECK(config_load(&store, &b, &seq));
    CHECK_MEM(&b, &a, sizeof(a));
    CHECK_EQ(seq, 1);
}

TEST(saves_alternate_slots) {
    sos_config_t a, b;
    uint32_t seq;
    blank_flash();
    sample(&a, "one");
    CHECK(config_save(&store, &a));             // seq 1 -> slot 0
    strcpy(a.wifi_ssid, "two");
    CHECK(config_save(&store, &a));             // seq 2 -> slot 1
    strcpy(a.wifi_ssid, "three");
    CHECK(config_save(&store, &a));             // seq 3 -> slot 0
    CHECK(config_load(&store, &b, &seq));
    CHECK(!strcmp(b.wifi_ssid, "three"));
    CHECK_EQ(seq, 3);
    CHECK_EQ(flash.writes, 3);
    CHECK(flash.slot[1][0] != 0xff);            // slot 1 still holds "two"
}

TEST(torn_write_keeps_previous_version) {
    sos_config_t a, b;
    blank_flash();
    sample(&a, "good");
    CHECK(config_save(&store, &a));
    strcpy(a.wifi_ssid, "interrupted");
    flash.tear_after = 100;                     // power cut mid-write
    CHECK(!config_save(&store, &a));            // read-back check notices
    CHECK(config_load(&store, &b, NULL));
    CHECK(!strcmp(b.wifi_ssid, "good"));
    strcpy(a.wifi_ssid, "retry");               // next save overwrites the torn slot again
    CHECK(config_save(&store, &a));
    CHECK(config_load(&store, &b, NULL));
    CHECK(!strcmp(b.wifi_ssid, "retry"));
}

TEST(failed_write_keeps_previous_version) {
    sos_config_t a, b;
    blank_flash();
    sample(&a, "kept");
    CHECK(config_save(&store, &a));
    strcpy(a.wifi_ssid, "lost");
    flash.fail = true;
    CHECK(!config_save(&store, &a));
    flash.fail = false;
    CHECK(config_load(&store, &b, NULL));
    CHECK(!strcmp(b.wifi_ssid, "kept"));
}

TEST(corrupt_newest_record_falls_back) {
    sos_config_t a, b;
    uint32_t seq;
    blank_flash();
    sample(&a, "older");
    CHECK(config_save(&store, &a));             // slot 0, seq 1
    strcpy(a.wifi_ssid, "newer");
    CHECK(config_save(&store, &a));             // slot 1, seq 2
    flash.slot[1][200] ^= 0x01;                 // one flipped bit in the payload
    CHECK(config_load(&store, &b, &seq));
    CHECK(!strcmp(b.wifi_ssid, "older"));
    CHECK_EQ(seq, 1);
}

TEST(both_corrupt_loads_defaults) {
    sos_config_t a, b;
    blank_flash();
    sample(&a, "x");
    CHECK(config_save(&store, &a));
    CHECK(config_save(&store, &a));
    flash.slot[0][20] ^= 0xff;
    flash.slot[1][20] ^= 0xff;
    CHECK(!config_load(&store, &b, NULL));
    CHECK(!strcmp(b.hostname, "sos"));
}

TEST(out_of_range_contents_rejected_even_with_valid_crc) {
    // A record whose CRC matches but whose key count is impossible must not be trusted.
    sos_config_t a, b;
    blank_flash();
    sample(&a, "x");
    a.key_count = CONFIG_MAX_KEYS + 1;
    CHECK(!config_save(&store, &a));            // written, but doesn't validate on read-back
    CHECK(!config_load(&store, &b, NULL));
    sample(&a, "x");
    memset(a.hostname, 'h', sizeof(a.hostname));   // unterminated string
    CHECK(!config_save(&store, &a));
    CHECK(!config_load(&store, &b, NULL));
}

TEST(key_count_beyond_array_rejected) {
    // key_count 9 with a valid CRC, and the bytes after the record forming a plausible 9th key
    // (as left by a larger record from other firmware): the count itself must be rejected.
    sos_config_t a, b;
    config_key_t k;
    blank_flash();
    sample(&a, "x");
    k = a.keys[0];
    while (a.key_count < CONFIG_MAX_KEYS) {     // fill all 8 entries, so only the count is wrong
        k.blob[k.len - 1]++;
        CHECK(config_key_add(&a, &k) == NULL);
    }
    CHECK(config_save(&store, &a));
    size_t count_off = 16 + offsetof(sos_config_t, key_count);
    flash.slot[0][count_off] = CONFIG_MAX_KEYS + 1;
    size_t extra = 16 + offsetof(sos_config_t, keys) + CONFIG_MAX_KEYS * sizeof(config_key_t);
    CHECK(extra + sizeof(config_key_t) <= CONFIG_SLOT_SIZE);
    memset(flash.slot[0] + extra, 0, sizeof(config_key_t));
    flash.slot[0][extra] = 1;                   // len = 1, comment empty and terminated
    fix_crc(0);
    CHECK(!config_load(&store, &b, NULL));
}

TEST(new_fields_round_trip) {
    sos_config_t a, b;
    blank_flash();
    sample(&a, "x");
    CHECK_EQ(a.ntp_sync_hours, 0);              // default: off
    a.ntp_sync_hours = 24;
    CHECK(config_save(&store, &a));
    CHECK(config_load(&store, &b, NULL));
    CHECK_EQ(b.ntp_sync_hours, 24);
    CHECK_EQ(b.ntp_server[0], 0);               // default: the build-time server

    strcpy(a.ntp_server, "time.example.org");
    CHECK(config_save(&store, &a));
    CHECK(config_load(&store, &b, NULL));
    CHECK(!strcmp(b.ntp_server, "time.example.org"));

    // Out-of-range values don't validate on read-back.
    sample(&a, "x");
    a.ntp_sync_hours = CONFIG_NTP_SYNC_HOURS_MAX + 1;
    CHECK(!config_save(&store, &a));
    sample(&a, "x");
    strcpy(a.ntp_server, "bad server");
    CHECK(!config_save(&store, &a));
    sample(&a, "x");
    memset(a.ntp_server, 'n', sizeof(a.ntp_server));   // unterminated
    CHECK(!config_save(&store, &a));
}

TEST(record_from_older_firmware_loads) {
    // A record saved by the first release is V1_LENGTH bytes long; whatever follows it in flash
    // (here 0xff, as in a sector erased before that write) must not leak into the new fields.
    sos_config_t a, b;
    uint32_t seq;
    CHECK_EQ(offsetof(sos_config_t, ntp_sync_hours), V1_LENGTH);
    blank_flash();
    sample(&a, "old");
    a.ntp_sync_hours = 0;
    CHECK(config_save(&store, &a));
    memset(flash.slot[0] + 16 + V1_LENGTH, 0xff, CONFIG_SLOT_SIZE - 16 - V1_LENGTH);
    set_length(0, V1_LENGTH);
    CHECK(config_load(&store, &b, &seq));
    CHECK_EQ(seq, 1);
    CHECK(!strcmp(b.wifi_ssid, "old"));
    CHECK_EQ(b.key_count, 1);
    CHECK_EQ(b.ntp_sync_hours, 0);
    CHECK_EQ(b.ntp_server[0], 0);

    // The next save writes the full current length, and an older record stays readable beside it.
    b.ntp_sync_hours = 12;
    strcpy(b.ntp_server, "10.0.0.1");
    CHECK(config_save(&store, &b));
    CHECK(config_load(&store, &a, &seq));
    CHECK_EQ(seq, 2);
    CHECK_EQ(a.ntp_sync_hours, 12);
    CHECK(!strcmp(a.ntp_server, "10.0.0.1"));
}

TEST(record_length_out_of_range_rejected) {
    sos_config_t a, b;
    blank_flash();
    sample(&a, "x");
    CHECK(config_save(&store, &a));
    set_length(0, V1_LENGTH - 1);               // shorter than any release wrote
    CHECK(!config_load(&store, &b, NULL));
    set_length(0, sizeof(sos_config_t) + 2);    // longer than this firmware knows
    CHECK(!config_load(&store, &b, NULL));
    set_length(0, V1_LENGTH + 2);               // between releases: would cut ntp_server off
    CHECK(!config_load(&store, &b, NULL));
    set_length(0, V2_LENGTH + 16);              // would cut certs_hash in half
    CHECK(!config_load(&store, &b, NULL));
    set_length(0, V3_LENGTH);                   // the third release's length
    CHECK(config_load(&store, &b, NULL));
    set_length(0, sizeof(sos_config_t));
    CHECK(config_load(&store, &b, NULL));
}

TEST(record_from_second_release_loads) {
    // The release with ntp_sync_hours and ntp_server wrote V2_LENGTH bytes; the trust-anchor hash
    // added after it must load as all zero ("none approved"), not as what follows in flash.
    sos_config_t a, b;
    CHECK_EQ(offsetof(sos_config_t, certs_hash), V2_LENGTH);
    blank_flash();
    sample(&a, "v2");
    a.ntp_sync_hours = 6;
    strcpy(a.ntp_server, "time.example.org");
    CHECK(config_save(&store, &a));
    memset(flash.slot[0] + 16 + V2_LENGTH, 0xff, CONFIG_SLOT_SIZE - 16 - V2_LENGTH);
    set_length(0, V2_LENGTH);
    CHECK(config_load(&store, &b, NULL));
    CHECK_EQ(b.ntp_sync_hours, 6);
    CHECK(!strcmp(b.ntp_server, "time.example.org"));
    uint8_t zero[CONFIG_CERTS_HASH_LEN] = {0};
    CHECK_MEM(b.certs_hash, zero, sizeof(zero));

    // A hash round-trips in a full-length record.
    memset(b.certs_hash, 0xab, sizeof(b.certs_hash));
    CHECK(config_save(&store, &b));
    CHECK(config_load(&store, &a, NULL));
    CHECK_MEM(a.certs_hash, b.certs_hash, sizeof(b.certs_hash));
}

TEST(record_from_third_release_loads) {
    // The release with certs_hash wrote V3_LENGTH bytes; Wi-Fi power saving, added after it, must
    // load as 0 (off), not as what follows in flash.
    sos_config_t a, b;
    CHECK_EQ(offsetof(sos_config_t, wifi_powersave), V3_LENGTH);
    blank_flash();
    sample(&a, "v3");
    memset(a.certs_hash, 0x5a, sizeof(a.certs_hash));
    CHECK(config_save(&store, &a));
    memset(flash.slot[0] + 16 + V3_LENGTH, 0xff, CONFIG_SLOT_SIZE - 16 - V3_LENGTH);
    set_length(0, V3_LENGTH);
    CHECK(config_load(&store, &b, NULL));
    CHECK_MEM(b.certs_hash, a.certs_hash, sizeof(a.certs_hash));
    CHECK_EQ(b.wifi_powersave, 0);

    // The setting round-trips in a full-length record.
    b.wifi_powersave = 1;
    CHECK(config_save(&store, &b));
    CHECK(config_load(&store, &a, NULL));
    CHECK_EQ(a.wifi_powersave, 1);
}

TEST(record_from_fourth_release_loads) {
    // The release with wifi_powersave wrote V4_LENGTH bytes; the country, added after it, must
    // load as "" (worldwide).
    sos_config_t a, b;
    CHECK_EQ(offsetof(sos_config_t, wifi_country), V4_LENGTH);
    blank_flash();
    sample(&a, "v4");
    a.wifi_powersave = 1;
    CHECK(config_save(&store, &a));
    memset(flash.slot[0] + 16 + V4_LENGTH, 0xff, CONFIG_SLOT_SIZE - 16 - V4_LENGTH);
    set_length(0, V4_LENGTH);
    CHECK(config_load(&store, &b, NULL));
    CHECK_EQ(b.wifi_powersave, 1);
    CHECK_EQ(b.wifi_country[0], 0);

    // The setting round-trips in a full-length record.
    strcpy(b.wifi_country, "US");
    CHECK(config_save(&store, &b));
    CHECK(config_load(&store, &a, NULL));
    CHECK(!strcmp(a.wifi_country, "US"));
}

TEST(rtc_offset_in_the_old_padding_byte) {
    // rtc_offset took the byte that was padding after wifi_powersave, so no record length
    // changed and the fields around it didn't move.
    sos_config_t a, b;
    CHECK_EQ(offsetof(sos_config_t, rtc_offset), V4_LENGTH - 1);
    CHECK_EQ(offsetof(sos_config_t, wifi_country), V4_LENGTH);
    blank_flash();
    sample(&a, "cal");
    CHECK_EQ(a.rtc_offset, 0);                  // default: none
    a.rtc_offset = -64;
    CHECK(config_save(&store, &a));
    CHECK(config_load(&store, &b, NULL));
    CHECK_EQ(b.rtc_offset, -64);
    b.rtc_offset = 63;
    CHECK(config_save(&store, &b));
    CHECK(config_load(&store, &a, NULL));
    CHECK_EQ(a.rtc_offset, 63);

    // A record from before the byte had a use could hold anything there. Out of range loads as
    // "none" and the rest of the record is kept.
    a.rtc_offset = 100;
    strcpy(a.wifi_country, "US");
    CHECK(config_save(&store, &a));
    CHECK(config_load(&store, &b, NULL));
    CHECK_EQ(b.rtc_offset, 0);
    CHECK(!strcmp(b.wifi_country, "US"));
    a.rtc_offset = -65;
    CHECK(config_save(&store, &a));
    CHECK(config_load(&store, &b, NULL));
    CHECK_EQ(b.rtc_offset, 0);

    // A record from the third release ends before the byte.
    blank_flash();
    sample(&a, "v3");
    a.rtc_offset = 21;
    CHECK(config_save(&store, &a));
    memset(flash.slot[0] + 16 + V3_LENGTH, 0xff, CONFIG_SLOT_SIZE - 16 - V3_LENGTH);
    set_length(0, V3_LENGTH);
    CHECK(config_load(&store, &b, NULL));
    CHECK_EQ(b.rtc_offset, 0);
}

TEST(wifi_country_checked) {
    CHECK(config_check_country("") == NULL);
    CHECK(config_check_country("US") == NULL);
    CHECK(config_check_country("us") != NULL);
    CHECK(config_check_country("U") != NULL);
    CHECK(config_check_country("USA") != NULL);
    sos_config_t a, b;
    blank_flash();
    sample(&a, "x");
    memcpy(a.wifi_country, "u1", 3);
    CHECK(!config_save(&store, &a));            // doesn't read back as valid
    CHECK(!config_load(&store, &b, NULL));
}

TEST(wifi_powersave_out_of_range_rejected) {
    sos_config_t a, b;
    blank_flash();
    sample(&a, "x");
    a.wifi_powersave = 2;
    CHECK(!config_save(&store, &a));            // doesn't read back as valid
    CHECK(!config_load(&store, &b, NULL));
}

TEST(sequence_wraps_around) {
    // Make slot 0 hold a valid record with seq 0xfffffffe, then save twice: the second save
    // wraps to 0, which must still count as newer than 0xffffffff.
    // Record layout: header magic(4) version(2) length(2) seq(4) crc(4), payload at offset 16;
    // the CRC covers the header up to crc and then the payload.
    sos_config_t a, b;
    uint32_t seq, big = 0xfffffffeu;
    blank_flash();
    sample(&a, "start");
    CHECK(config_save(&store, &a));
    memcpy(flash.slot[0] + 8, &big, 4);
    fix_crc(0);
    CHECK(config_load(&store, &b, &seq));
    CHECK_EQ(seq, 0xfffffffeu);
    strcpy(a.wifi_ssid, "max");
    CHECK(config_save(&store, &a));             // slot 1, seq 0xffffffff
    strcpy(a.wifi_ssid, "wrapped");
    CHECK(config_save(&store, &a));             // slot 0, seq 0
    CHECK(config_load(&store, &b, &seq));
    CHECK_EQ(seq, 0);
    CHECK(!strcmp(b.wifi_ssid, "wrapped"));
}

TEST(erase_resets_to_defaults) {
    sos_config_t a, b;
    blank_flash();
    sample(&a, "x");
    CHECK(config_save(&store, &a));
    CHECK(config_save(&store, &a));
    CHECK(config_erase(&store));
    CHECK(!config_load(&store, &b, NULL));
    CHECK_EQ(b.key_count, 0);
    CHECK(config_save(&store, &a));             // usable again afterwards, starting at seq 1
    uint32_t seq;
    CHECK(config_load(&store, &b, &seq));
    CHECK_EQ(seq, 1);
}

// --- validation ---------------------------------------------------------------------------------

TEST(ssid_rules) {
    CHECK(config_check_ssid("home") == NULL);
    CHECK(config_check_ssid("12345678901234567890123456789012") == NULL);    // 32
    CHECK(config_check_ssid("123456789012345678901234567890123") != NULL);   // 33
    CHECK(config_check_ssid("") != NULL);
}

TEST(password_rules) {
    CHECK(config_check_password("") == NULL);                  // open network
    CHECK(config_check_password("12345678") == NULL);
    CHECK(config_check_password("1234567") != NULL);           // too short for WPA2
    char p[65];
    memset(p, 'p', 63); p[63] = '\0';
    CHECK(config_check_password(p) == NULL);
    p[63] = 'p'; p[64] = '\0';
    CHECK(config_check_password(p) != NULL);                   // 64
    CHECK(config_check_password("abcdefg\x01") != NULL);       // control character
}

TEST(hostname_rules) {
    CHECK(config_check_hostname("sos") == NULL);
    CHECK(config_check_hostname("pico-2w") == NULL);
    CHECK(config_check_hostname("-pico") != NULL);
    CHECK(config_check_hostname("pico-") != NULL);
    CHECK(config_check_hostname("pi co") != NULL);
    CHECK(config_check_hostname("pico.local") != NULL);
    CHECK(config_check_hostname("") != NULL);
}

TEST(ntp_server_rules) {
    CHECK(config_check_ntp_server("pool.ntp.org") == NULL);
    CHECK(config_check_ntp_server("time-a.example") == NULL);
    CHECK(config_check_ntp_server("192.168.1.1") == NULL);
    CHECK(config_check_ntp_server("ntp") == NULL);
    CHECK(config_check_ntp_server("") != NULL);
    CHECK(config_check_ntp_server(".pool.ntp.org") != NULL);
    CHECK(config_check_ntp_server("pool.ntp.org.") != NULL);
    CHECK(config_check_ntp_server("pool..ntp.org") != NULL);
    CHECK(config_check_ntp_server("pool ntp.org") != NULL);
    CHECK(config_check_ntp_server("pool.ntp.org:123") != NULL);
    char longest[CONFIG_NTP_SERVER_MAX + 2];
    memset(longest, 'a', CONFIG_NTP_SERVER_MAX);
    longest[CONFIG_NTP_SERVER_MAX] = '\0';
    CHECK(config_check_ntp_server(longest) == NULL);
    longest[CONFIG_NTP_SERVER_MAX] = 'a';
    longest[CONFIG_NTP_SERVER_MAX + 1] = '\0';
    CHECK(config_check_ntp_server(longest) != NULL);
}

TEST(user_rules) {
    CHECK(config_check_user("julio") == NULL);
    CHECK(config_check_user("j.m_2-x") == NULL);
    CHECK(config_check_user("") != NULL);
    CHECK(config_check_user("bad user") != NULL);
    CHECK(config_check_user("root;rm") != NULL);
}

// --- base64 -------------------------------------------------------------------------------------

TEST(base64_known_values) {
    uint8_t out[16];
    CHECK_EQ(config_base64_decode("", 0, out, sizeof(out)), 0);
    CHECK_EQ(config_base64_decode("Zg==", 4, out, sizeof(out)), 1);
    CHECK_MEM(out, "f", 1);
    CHECK_EQ(config_base64_decode("Zm8=", 4, out, sizeof(out)), 2);
    CHECK_MEM(out, "fo", 2);
    CHECK_EQ(config_base64_decode("Zm9vYmFy", 8, out, sizeof(out)), 6);
    CHECK_MEM(out, "foobar", 6);
    CHECK_EQ(config_base64_decode("+/+/", 4, out, sizeof(out)), 3);
    CHECK_MEM(out, "\xfb\xff\xbf", 3);
}

TEST(base64_rejects_malformed) {
    uint8_t out[16];
    CHECK_EQ(config_base64_decode("Zm9", 3, out, sizeof(out)), -1);         // not a multiple of 4
    CHECK_EQ(config_base64_decode("Zm9v!mFy", 8, out, sizeof(out)), -1);    // bad character
    CHECK_EQ(config_base64_decode("Zg==Zm9v", 8, out, sizeof(out)), -1);    // padding mid-way
    CHECK_EQ(config_base64_decode("Z===", 4, out, sizeof(out)), -1);        // too much padding
    CHECK_EQ(config_base64_decode("Zm=v", 4, out, sizeof(out)), -1);        // data after '='
    CHECK_EQ(config_base64_decode("Zm9vYmFy", 8, out, 5), -1);              // doesn't fit
}

// --- keys ---------------------------------------------------------------------------------------

TEST(parse_ed25519) {
    config_key_t k;
    char type[32];
    CHECK(config_key_parse(ED25519_LINE, &k) == NULL);
    CHECK_EQ(k.len, 51);
    CHECK(!strcmp(k.comment, "alice@laptop"));
    CHECK(!strcmp(config_key_type(&k, type, sizeof(type)), "ssh-ed25519"));
}

TEST(parse_ecdsa) {
    config_key_t k;
    char type[32];
    CHECK(config_key_parse(ECDSA_LINE, &k) == NULL);
    CHECK_EQ(k.len, 104);
    CHECK(!strcmp(k.comment, "bob@desk"));
    CHECK(!strcmp(config_key_type(&k, type, sizeof(type)), "ecdsa-sha2-nistp256"));
}

TEST(parse_without_comment_and_with_extra_spaces) {
    config_key_t k;
    CHECK(config_key_parse("  ssh-ed25519   AAAAC3NzaC1lZDI1NTE5AAAAIJK3YOM3IDvlmN1xnYQm8HBTPfNlEa0FyWLOgTLqQt+x  ", &k) == NULL);
    CHECK_EQ(k.len, 51);
    CHECK_EQ(k.comment[0], 0);
    CHECK(config_key_parse(ED25519_LINE "\r\n", &k) == NULL);
    CHECK(!strcmp(k.comment, "alice@laptop"));
}

// Minimal base64 encoder, to build crafted key lines.
static void b64_encode(const uint8_t *in, size_t n, char *out) {
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        *out++ = a[v >> 18 & 63];
        *out++ = a[v >> 12 & 63];
        *out++ = i + 1 < n ? a[v >> 6 & 63] : '=';
        *out++ = i + 2 < n ? a[v & 63] : '=';
    }
    *out = '\0';
}

TEST(parse_rejects_mismatched_embedded_type) {
    // A blob of exactly the ed25519 length whose embedded type name differs ("ssh-ed25518").
    config_key_t k;
    char line[200] = "ssh-ed25519 ";
    CHECK(config_key_parse(ED25519_LINE, &k) == NULL);
    k.blob[4 + 10] = '8';
    b64_encode(k.blob, k.len, line + strlen(line));
    CHECK(config_key_parse(line, &k) != NULL);
}

TEST(long_comment_is_truncated) {
    config_key_t k;
    CHECK(config_key_parse(ED25519_LINE " with a very long description that goes on and on", &k) == NULL);
    CHECK_EQ(strlen(k.comment), CONFIG_KEY_COMMENT_MAX);
}

TEST(parse_rejects_bad_keys) {
    config_key_t k;
    CHECK(config_key_parse(RSA_LINE, &k) != NULL);                          // unsupported type
    CHECK(config_key_parse("ssh-ed25519", &k) != NULL);                     // no key data
    CHECK(config_key_parse("", &k) != NULL);
    CHECK(config_key_parse("ssh-ed25519 not*base64 x", &k) != NULL);
    // Type in front doesn't match the type inside the blob.
    CHECK(config_key_parse("ecdsa-sha2-nistp256 AAAAC3NzaC1lZDI1NTE5AAAAIJK3YOM3IDvlmN1xnYQm8HBTPfNlEa0FyWLOgTLqQt+x", &k) != NULL);
    // Valid base64 of the right type, truncated: wrong length.
    CHECK(config_key_parse("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIJK3YOM3IDvlmN1xnYQm8HBTPfNlEa0F", &k) != NULL);
    CHECK_EQ(k.len, 0);                                                     // left cleared
}

TEST(key_list_add_find_remove) {
    sos_config_t c;
    config_key_t ed, ec;
    config_defaults(&c);
    CHECK(config_key_parse(ED25519_LINE, &ed) == NULL);
    CHECK(config_key_parse(ECDSA_LINE, &ec) == NULL);
    CHECK(config_key_add(&c, &ed) == NULL);
    CHECK(config_key_add(&c, &ec) == NULL);
    CHECK(config_key_add(&c, &ed) != NULL);                                 // duplicate
    CHECK_EQ(c.key_count, 2);
    CHECK(config_key_find(&c, ed.blob, ed.len));
    CHECK(config_key_find(&c, ec.blob, ec.len));
    CHECK(!config_key_find(&c, ed.blob, ed.len - 1));                       // prefix isn't a match
    CHECK(config_key_remove(&c, 0));
    CHECK_EQ(c.key_count, 1);
    CHECK(!config_key_find(&c, ed.blob, ed.len));
    CHECK(!strcmp(c.keys[0].comment, "bob@desk"));                          // order kept
    CHECK(!config_key_remove(&c, 1));                                       // out of range
    CHECK(config_key_remove(&c, 0));
    CHECK_EQ(c.key_count, 0);
}

TEST(remove_keeps_order) {
    sos_config_t c;
    config_key_t k;
    config_defaults(&c);
    CHECK(config_key_parse(ED25519_LINE, &k) == NULL);
    for (int i = 0; i < 3; i++) {
        k.blob[k.len - 1] = (uint8_t)i;
        k.comment[0] = (char)('a' + i);
        k.comment[1] = '\0';
        CHECK(config_key_add(&c, &k) == NULL);
    }
    CHECK(config_key_remove(&c, 0));
    CHECK_EQ(c.key_count, 2);
    CHECK(!strcmp(c.keys[0].comment, "b"));
    CHECK(!strcmp(c.keys[1].comment, "c"));
}

TEST(key_list_capacity) {
    sos_config_t c;
    config_key_t k;
    config_defaults(&c);
    CHECK(config_key_parse(ED25519_LINE, &k) == NULL);
    for (int i = 0; i < CONFIG_MAX_KEYS; i++) {
        k.blob[k.len - 1] = (uint8_t)i;                                     // distinct keys
        CHECK(config_key_add(&c, &k) == NULL);
    }
    k.blob[k.len - 1] = 0xee;
    CHECK(config_key_add(&c, &k) != NULL);                                  // full
    CHECK_EQ(c.key_count, CONFIG_MAX_KEYS);
}

TEST(crc32_known_value) {
    CHECK_EQ(config_crc32("123456789", 9), 0xcbf43926u);                    // standard check value
}

// --- sealed records ---------------------------------------------------------------------------------
//
// config.c seals through sealer_t and knows no cryptography, so these tests use a fake sealer
// that keeps the rules a real one has: the blob is 28 bytes longer than the data, the data
// isn't readable in it, and it opens only with the same key, the same associated data and
// unchanged bytes. The real AES-256-GCM sealer is tested with config.c in test_seal.c.

typedef struct {
    uint8_t key;
    uint8_t next_nonce;
    bool fail;
    int seals, opens;
} fake_sealer_t;

static uint8_t fake_sum(const fake_sealer_t *f, const uint8_t *aad, size_t aad_len,
                        const uint8_t *plain, size_t len, uint8_t nonce) {
    uint32_t h = 2166136261u ^ f->key ^ ((uint32_t)nonce << 8);
    for (size_t i = 0; i < aad_len; i++)
        h = (h ^ aad[i]) * 16777619u;
    for (size_t i = 0; i < len; i++)
        h = (h ^ plain[i]) * 16777619u;
    return (uint8_t)(h ^ (h >> 8) ^ (h >> 16) ^ (h >> 24));
}

static bool fake_seal(void *ctx, const uint8_t *aad, size_t aad_len, const uint8_t *plain,
                      size_t len, uint8_t *out) {
    fake_sealer_t *f = ctx;
    f->seals++;
    if (f->fail) {
        memset(out, 0, len + SEALER_OVERHEAD);
        return false;
    }
    uint8_t nonce = f->next_nonce++;
    memset(out, nonce, SEALER_NONCE_LEN);
    for (size_t i = 0; i < len; i++)
        out[SEALER_NONCE_LEN + i] = plain[i] ^ (uint8_t)(f->key + nonce + i);
    memset(out + SEALER_NONCE_LEN + len, fake_sum(f, aad, aad_len, plain, len, nonce),
           SEALER_TAG_LEN);
    return true;
}

static bool fake_open(void *ctx, const uint8_t *aad, size_t aad_len, const uint8_t *sealed,
                      size_t sealed_len, uint8_t *plain) {
    fake_sealer_t *f = ctx;
    f->opens++;
    size_t len = sealed_len - SEALER_OVERHEAD;
    uint8_t nonce = sealed[0];
    for (size_t i = 0; i < len; i++)
        plain[i] = sealed[SEALER_NONCE_LEN + i] ^ (uint8_t)(f->key + nonce + i);
    uint8_t sum = fake_sum(f, aad, aad_len, plain, len, nonce);
    bool ok = !f->fail;
    for (size_t i = 0; i < SEALER_NONCE_LEN; i++)
        ok = ok && sealed[i] == nonce;
    for (size_t i = 0; i < SEALER_TAG_LEN; i++)
        ok = ok && sealed[SEALER_NONCE_LEN + len + i] == sum;
    if (!ok)
        memset(plain, 0, len);
    return ok;
}

static fake_sealer_t fake = { .key = 0x5a };
static const sealer_t fake_sealer = { fake_seal, fake_open, &fake };
// The device's store on an open board, and on a secured one.
static config_store_t sealed_store = { flash_read, flash_write, &flash, &fake_sealer, false };
static config_store_t secured_store = { flash_read, flash_write, &flash, &fake_sealer, true };

static void sealed_start(void) {
    blank_flash();
    memset(&fake, 0, sizeof(fake));
    fake.key = 0x5a;
    fake.next_nonce = 1;
}

static uint16_t slot_version(unsigned slot) {
    uint16_t v;
    memcpy(&v, flash.slot[slot] + 4, 2);
    return v;
}

static uint32_t slot_seq(unsigned slot) {
    uint32_t v;
    memcpy(&v, flash.slot[slot] + 8, 4);
    return v;
}

static bool slot_contains(unsigned slot, const char *text) {
    size_t n = strlen(text);
    for (size_t i = 0; i + n <= CONFIG_SLOT_SIZE; i++)
        if (!memcmp(flash.slot[slot] + i, text, n))
            return true;
    return false;
}

// A slot after config.c blanked it: a zeroed header and the rest erased.
static bool slot_is_blanked(unsigned slot) {
    for (size_t i = 0; i < CONFIG_SLOT_SIZE; i++)
        if (flash.slot[slot][i] != (i < 16 ? 0 : 0xff))
            return false;
    return true;
}

// Recompute the CRC of a sealed record after a test patched it (header up to the CRC, then the
// sealed payload: length + 28 bytes).
static void fix_sealed_crc(unsigned slot) {
    static uint8_t crc_input[12 + CONFIG_SLOT_SIZE];
    uint16_t length;
    memcpy(&length, flash.slot[slot] + 6, 2);
    size_t stored = (size_t)length + SEALER_OVERHEAD;
    memcpy(crc_input, flash.slot[slot], 12);
    memcpy(crc_input + 12, flash.slot[slot] + 16, stored);
    uint32_t crc = config_crc32(crc_input, 12 + stored);
    memcpy(flash.slot[slot] + 12, &crc, 4);
}

TEST(sealed_save_and_load_round_trip) {
    sos_config_t a, b;
    uint32_t seq = 0;
    sealed_start();
    sample(&a, "HomeNet");
    CHECK(config_save(&sealed_store, &a));
    CHECK_EQ(config_stored(&sealed_store), CONFIG_STORED_SEALED);
    CHECK(config_load(&sealed_store, &b, &seq));
    CHECK_EQ(seq, 1);
    CHECK_MEM(&a, &b, sizeof(a));
    CHECK(!config_sealed_unopened(&sealed_store));
}

// The stored layout: the same 16-byte header with version 2 and the configuration's length,
// then length + 28 bytes; nothing of the configuration readable.
TEST(sealed_record_layout) {
    sos_config_t a;
    sealed_start();
    sample(&a, "HomeNet");
    CHECK(config_save(&sealed_store, &a));
    static const uint8_t magic[4] = { 'S', 'O', 'S', 'C' };
    CHECK_MEM(flash.slot[0], magic, 4);
    CHECK_EQ(slot_version(0), 2);
    uint16_t length;
    memcpy(&length, flash.slot[0] + 6, 2);
    CHECK_EQ(length, sizeof(sos_config_t));
    CHECK_EQ(slot_seq(0), 1);
    for (size_t i = 16 + sizeof(sos_config_t) + SEALER_OVERHEAD; i < CONFIG_SLOT_SIZE; i++)
        CHECK_EQ(flash.slot[0][i], 0xff);
    CHECK(!slot_contains(0, "HomeNet"));
    CHECK(!slot_contains(0, "correct horse"));
    CHECK(!slot_contains(0, "alice@laptop"));
}

TEST(sealed_saves_alternate_slots_with_fresh_nonces) {
    sos_config_t a, b;
    uint32_t seq = 0;
    sealed_start();
    sample(&a, "HomeNet");
    CHECK(config_save(&sealed_store, &a));          // seq 1 -> slot 0
    CHECK(config_save(&sealed_store, &a));          // seq 2 -> slot 1
    CHECK(flash.slot[0][16] != flash.slot[1][16]);  // the sealer was asked for a new seal
    strcpy(a.hostname, "third");
    CHECK(config_save(&sealed_store, &a));          // seq 3 -> slot 0
    CHECK(config_load(&sealed_store, &b, &seq));
    CHECK_EQ(seq, 3);
    CHECK(!strcmp(b.hostname, "third"));
}

TEST(sealed_torn_write_keeps_previous_version) {
    sos_config_t a, b;
    sealed_start();
    sample(&a, "first");
    CHECK(config_save(&sealed_store, &a));
    strcpy(a.wifi_ssid, "second");
    flash.tear_after = 700;
    CHECK(!config_save(&sealed_store, &a));
    CHECK(config_load(&sealed_store, &b, NULL));
    CHECK(!strcmp(b.wifi_ssid, "first"));
}

TEST(save_fails_when_sealing_fails) {
    sos_config_t a, b;
    sealed_start();
    sample(&a, "first");
    CHECK(config_save(&sealed_store, &a));
    int writes = flash.writes;
    strcpy(a.wifi_ssid, "second");
    fake.fail = true;                               // no key, or no random numbers
    CHECK(!config_save(&sealed_store, &a));
    CHECK_EQ(flash.writes, writes);                 // nothing was written, sealed or plain
    fake.fail = false;
    CHECK(config_load(&sealed_store, &b, NULL));
    CHECK(!strcmp(b.wifi_ssid, "first"));
}

// Any change to a sealed record makes it no record: the payload, the tag, or a header field
// (even with the CRC put right, as someone rewriting the flash would).
TEST(changed_sealed_record_does_not_load) {
    sos_config_t a, b;
    sealed_start();
    sample(&a, "HomeNet");
    CHECK(config_save(&sealed_store, &a));
    static uint8_t good[CONFIG_SLOT_SIZE];
    memcpy(good, flash.slot[0], sizeof(good));
    size_t tag = 16 + SEALER_NONCE_LEN + sizeof(sos_config_t);

    flash.slot[0][16 + SEALER_NONCE_LEN + 40] ^= 1;         // a ciphertext bit
    CHECK(!config_load(&sealed_store, &b, NULL));           // the CRC notices
    fix_sealed_crc(0);
    CHECK(!config_load(&sealed_store, &b, NULL));           // and so does the seal
    CHECK(config_sealed_unopened(&sealed_store));

    memcpy(flash.slot[0], good, sizeof(good));
    flash.slot[0][tag] ^= 1;                                // the tag
    fix_sealed_crc(0);
    CHECK(!config_load(&sealed_store, &b, NULL));

    memcpy(flash.slot[0], good, sizeof(good));
    flash.slot[0][16] ^= 1;                                 // the nonce
    fix_sealed_crc(0);
    CHECK(!config_load(&sealed_store, &b, NULL));

    memcpy(flash.slot[0], good, sizeof(good));
    uint32_t seq = 99;                                      // the sequence number
    memcpy(flash.slot[0] + 8, &seq, 4);
    fix_sealed_crc(0);
    CHECK(!config_load(&sealed_store, &b, NULL));
    CHECK(config_sealed_unopened(&sealed_store));

    memcpy(flash.slot[0], good, sizeof(good));              // unchanged: loads again
    CHECK(config_load(&sealed_store, &b, NULL));
    CHECK_MEM(&a, &b, sizeof(a));
}

// An old sealed record can't be made the newest by giving it a higher sequence number.
TEST(sealed_rollback_by_sequence_number_fails) {
    sos_config_t a, b;
    uint32_t seq = 0;
    sealed_start();
    sample(&a, "old");
    CHECK(config_save(&sealed_store, &a));          // slot 0, seq 1
    strcpy(a.wifi_ssid, "new");
    CHECK(config_save(&sealed_store, &a));          // slot 1, seq 2
    uint32_t forged = 7;
    memcpy(flash.slot[0] + 8, &forged, 4);
    fix_sealed_crc(0);
    CHECK(config_load(&sealed_store, &b, &seq));
    CHECK_EQ(seq, 2);
    CHECK(!strcmp(b.wifi_ssid, "new"));
}

TEST(sealed_under_another_key_does_not_load) {
    sos_config_t a, b;
    sealed_start();
    sample(&a, "HomeNet");
    CHECK(config_save(&sealed_store, &a));
    fake.key = 0x11;                                // another root secret
    memset(&b, 0x55, sizeof(b));
    CHECK(!config_load(&sealed_store, &b, NULL));
    CHECK(!strcmp(b.hostname, "sos"));              // defaults
    CHECK_EQ(b.key_count, 0);
    CHECK_EQ(config_stored(&sealed_store), CONFIG_STORED_NONE);
    CHECK(config_sealed_unopened(&sealed_store));   // and it says why
    static uint8_t before[2][CONFIG_SLOT_SIZE];
    memcpy(before, flash.slot, sizeof(before));
    CHECK(config_scrub_plain(&sealed_store));       // nothing is erased on its own
    CHECK_MEM(flash.slot, before, sizeof(before));
}

TEST(sealed_contents_out_of_range_rejected) {
    sos_config_t a, b;
    sealed_start();
    sample(&a, "HomeNet");
    a.key_count = CONFIG_MAX_KEYS + 1;
    CHECK(!config_save(&sealed_store, &a));         // sealed fine, but doesn't validate
    CHECK(!config_load(&sealed_store, &b, NULL));
}

// A store without a sealer (the provisioning tool) saves plain and can't use sealed records.
TEST(store_without_sealer_ignores_sealed_records) {
    sos_config_t a, b;
    sealed_start();
    sample(&a, "HomeNet");
    CHECK(config_save(&sealed_store, &a));
    CHECK(!config_load(&store, &b, NULL));
    CHECK_EQ(config_stored(&store), CONFIG_STORED_NONE);
    CHECK(config_save(&store, &a));
    CHECK_EQ(slot_version(0), 1);
    CHECK(config_scrub_plain(&store));              // and never erases anything
    CHECK_EQ(slot_version(0), 1);
}

// --- plain records on a board that seals --------------------------------------------------------------

// A plain record, as older firmware saved it or the provisioning tool wrote it: slot 0, seq 1.
static void plain_record(const char *ssid) {
    sos_config_t a;
    sample(&a, ssid);
    CHECK(config_save(&store, &a));
}

// Upgrade and provisioning: the plain record loads, the next save stores it sealed, and the
// plain copy is erased so the Wi-Fi password isn't left readable.
TEST(plain_record_loads_then_is_sealed_and_erased) {
    sos_config_t a, b;
    uint32_t seq = 0;
    sealed_start();
    plain_record("HomeNet");
    CHECK(slot_contains(0, "correct horse"));
    CHECK_EQ(config_stored(&sealed_store), CONFIG_STORED_PLAIN);
    CHECK(config_load(&sealed_store, &a, &seq));
    CHECK_EQ(seq, 1);
    CHECK(!strcmp(a.wifi_ssid, "HomeNet"));

    CHECK(config_save(&sealed_store, &a));
    CHECK_EQ(config_stored(&sealed_store), CONFIG_STORED_SEALED);
    CHECK_EQ(slot_version(1), 2);
    CHECK_EQ(slot_seq(1), 2);
    CHECK(slot_is_blanked(0));
    CHECK(!slot_contains(0, "correct horse"));
    CHECK(!slot_contains(1, "correct horse"));
    CHECK(config_load(&sealed_store, &b, &seq));
    CHECK_EQ(seq, 2);
    CHECK_MEM(&a, &b, sizeof(a));
    CHECK(config_scrub_plain(&sealed_store));
}

// A record of every released length, saved plain by its firmware, is sealed the same way.
TEST(old_plain_record_is_sealed_with_its_fields) {
    sos_config_t a, b;
    sealed_start();
    plain_record("HomeNet");
    set_length(0, V1_LENGTH);                       // as the first release saved it
    CHECK(config_load(&sealed_store, &a, NULL));
    CHECK(!strcmp(a.wifi_ssid, "HomeNet"));
    CHECK_EQ(a.ntp_sync_hours, 0);
    CHECK(config_save(&sealed_store, &a));
    CHECK(slot_is_blanked(0));
    CHECK(config_load(&sealed_store, &b, NULL));
    CHECK_MEM(&a, &b, sizeof(a));
}

// Power is lost after the sealed record is written and before the plain one is erased. The
// plain record is still the current one at the next boot (same contents), so the board seals
// it again, above both sequence numbers, and this time erases it.
TEST(an_interrupted_sealing_is_done_again) {
    sos_config_t a, b;
    uint32_t seq = 0;
    sealed_start();
    plain_record("HomeNet");
    CHECK(config_load(&sealed_store, &a, NULL));
    static uint8_t plain[CONFIG_SLOT_SIZE];
    memcpy(plain, flash.slot[0], sizeof(plain));
    CHECK(config_save(&sealed_store, &a));          // sealed in slot 1, seq 2
    memcpy(flash.slot[0], plain, sizeof(plain));    // as if the erase never happened

    CHECK_EQ(config_stored(&sealed_store), CONFIG_STORED_PLAIN);
    CHECK(!config_scrub_plain(&sealed_store));      // the scrub leaves the current record alone
    CHECK(slot_contains(0, "correct horse"));
    CHECK(config_load(&sealed_store, &b, NULL));
    CHECK_MEM(&a, &b, sizeof(a));
    CHECK(config_save(&sealed_store, &b));          // what settings_init() does next
    CHECK(slot_is_blanked(0));
    CHECK_EQ(config_stored(&sealed_store), CONFIG_STORED_SEALED);
    CHECK(config_load(&sealed_store, &b, &seq));
    CHECK_EQ(seq, 3);
    CHECK_MEM(&a, &b, sizeof(a));
    CHECK(config_scrub_plain(&sealed_store));
}

// Downgrade, set the board up again under the older firmware, upgrade. The older firmware
// can't read the sealed record, so its save is a plain record numbered 1, in slot 0, next to a
// sealed record with a higher number. The configuration just set up must win, not the old
// sealed one, and must not be erased as a leftover.
TEST(plain_record_saved_after_a_downgrade_wins) {
    sos_config_t a, b;
    uint32_t seq = 0;
    sealed_start();
    plain_record("Before");
    CHECK(config_load(&sealed_store, &a, NULL));
    CHECK(config_save(&sealed_store, &a));          // upgrade: sealed in slot 1, seq 2
    strcpy(a.hostname, "saved-twice");
    CHECK(config_save(&sealed_store, &a));          // slot 0, seq 3
    CHECK(config_save(&sealed_store, &a));          // slot 1, seq 4
    CHECK_EQ(slot_version(1), 2);
    CHECK_EQ(slot_seq(1), 4);

    // The older firmware: it sees no record it knows, and saves to slot 0 with number 1.
    CHECK(!config_load(&store, &b, NULL));
    sample(&b, "AfterDowngrade");
    CHECK(config_save(&store, &b));
    CHECK_EQ(slot_version(0), 1);
    CHECK_EQ(slot_seq(0), 1);

    // Upgrade again.
    CHECK_EQ(config_stored(&sealed_store), CONFIG_STORED_PLAIN);
    CHECK(config_load(&sealed_store, &a, &seq));
    CHECK(!strcmp(a.wifi_ssid, "AfterDowngrade"));
    CHECK(!config_scrub_plain(&sealed_store));      // not a leftover: it's the configuration
    CHECK(config_load(&sealed_store, &a, NULL));
    CHECK(!strcmp(a.wifi_ssid, "AfterDowngrade"));
    CHECK(config_save(&sealed_store, &a));          // sealed, numbered above both records
    CHECK_EQ(config_stored(&sealed_store), CONFIG_STORED_SEALED);
    CHECK(config_load(&sealed_store, &b, &seq));
    CHECK_EQ(seq, 5);
    CHECK(!strcmp(b.wifi_ssid, "AfterDowngrade"));
    CHECK(slot_is_blanked(0));
    CHECK(!slot_contains(0, "AfterDowngrade") || slot_version(0) == 2);
    CHECK(config_scrub_plain(&sealed_store));
}

// The same flash on a secured board: the plain record doesn't exist for it.
TEST(secured_board_ignores_a_plain_record_saved_after_a_downgrade) {
    sos_config_t a, b;
    uint32_t seq = 0;
    sealed_start();
    sample(&a, "Sealed");
    CHECK(config_save(&secured_store, &a));         // slot 0, seq 1
    CHECK(config_save(&secured_store, &a));         // slot 1, seq 2
    static uint8_t keep[CONFIG_SLOT_SIZE];
    memcpy(keep, flash.slot[1], sizeof(keep));
    blank_flash();
    plain_record("Planted");                        // slot 0, plain, seq 1
    memcpy(flash.slot[1], keep, sizeof(keep));
    CHECK(config_load(&secured_store, &b, &seq));
    CHECK_EQ(seq, 2);
    CHECK(!strcmp(b.wifi_ssid, "Sealed"));
    CHECK(config_scrub_plain(&secured_store));      // and the planted record is erased
    CHECK(slot_is_blanked(0));
}

// The only copy of the configuration is never erased: a plain record with no sealed one.
TEST(scrub_keeps_a_plain_record_that_is_the_only_copy) {
    sos_config_t a;
    sealed_start();
    plain_record("HomeNet");
    CHECK(!config_scrub_plain(&sealed_store));      // "a plain record is still there"
    CHECK(config_load(&sealed_store, &a, NULL));
    CHECK(!strcmp(a.wifi_ssid, "HomeNet"));
    // Nor when the other slot holds a sealed record that doesn't open.
    CHECK(config_save(&sealed_store, &a));          // slot 1 sealed, slot 0 blanked
    plain_record("Other");                          // a plain record again, in slot 0, seq 3
    fake.key = 0x11;
    CHECK(!config_scrub_plain(&sealed_store));
    CHECK(config_load(&sealed_store, &a, NULL));
    CHECK(!strcmp(a.wifi_ssid, "Other"));
}

TEST(failed_erase_of_the_plain_copy_is_reported) {
    sos_config_t a;
    sealed_start();
    plain_record("HomeNet");
    CHECK(config_load(&sealed_store, &a, NULL));
    static uint8_t plain[CONFIG_SLOT_SIZE];
    memcpy(plain, flash.slot[0], sizeof(plain));
    CHECK(config_save(&sealed_store, &a));
    memcpy(flash.slot[0], plain, sizeof(plain));
    flash.slot[0][12] ^= 1;                         // a leftover that no longer loads
    flash.fail = true;
    CHECK(!config_scrub_plain(&sealed_store));
    flash.fail = false;
    CHECK(config_scrub_plain(&sealed_store));
    CHECK(slot_is_blanked(0));
}

// A damaged plain record next to the sealed configuration is erased too: it may still hold the
// password.
TEST(scrub_erases_a_damaged_plain_record) {
    sos_config_t a;
    sealed_start();
    plain_record("HomeNet");
    CHECK(config_load(&sealed_store, &a, NULL));
    static uint8_t plain[CONFIG_SLOT_SIZE];
    memcpy(plain, flash.slot[0], sizeof(plain));
    CHECK(config_save(&sealed_store, &a));
    memcpy(flash.slot[0], plain, sizeof(plain));
    flash.slot[0][12] ^= 1;                         // its CRC no longer matches
    CHECK(slot_contains(0, "correct horse"));
    CHECK(config_scrub_plain(&sealed_store));
    CHECK(slot_is_blanked(0));
}

// --- a secured board: plain records don't exist --------------------------------------------------------

TEST(secured_board_does_not_load_a_plain_record) {
    sos_config_t a;
    sealed_start();
    plain_record("Planted");
    memset(&a, 0x55, sizeof(a));
    CHECK(!config_load(&secured_store, &a, NULL));
    CHECK(!strcmp(a.hostname, "sos"));
    CHECK_EQ(a.key_count, 0);
    CHECK_EQ(config_stored(&secured_store), CONFIG_STORED_NONE);
}

// A planted plain record with a high sequence number must not hide the sealed configuration.
TEST(secured_board_plain_record_cannot_hide_the_sealed_one) {
    sos_config_t a, b;
    uint32_t seq = 0;
    sealed_start();
    sample(&a, "Real");
    CHECK(config_save(&secured_store, &a));         // slot 0, seq 1, sealed
    // The attacker writes a plain record into slot 1 with a much higher sequence number.
    static uint8_t keep[CONFIG_SLOT_SIZE];
    memcpy(keep, flash.slot[0], sizeof(keep));
    blank_flash();
    plain_record("Planted");                        // slot 0, plain
    memcpy(flash.slot[1], flash.slot[0], CONFIG_SLOT_SIZE);
    uint32_t high = 1000;
    memcpy(flash.slot[1] + 8, &high, 4);
    fix_crc(1);
    memcpy(flash.slot[0], keep, sizeof(keep));

    CHECK_EQ(config_stored(&secured_store), CONFIG_STORED_SEALED);
    CHECK(config_load(&secured_store, &b, &seq));
    CHECK_EQ(seq, 1);
    CHECK(!strcmp(b.wifi_ssid, "Real"));
    // The next save takes the sealed record's number, not the planted one's, and the planted
    // record is erased.
    CHECK(config_save(&secured_store, &b));
    CHECK(config_load(&secured_store, &b, &seq));
    CHECK_EQ(seq, 2);
    CHECK(!strcmp(b.wifi_ssid, "Real"));
    CHECK(!slot_contains(0, "Planted") && !slot_contains(1, "Planted"));
    // On an open board the same flash would load the planted record: that's what "open" means.
    memcpy(flash.slot[0], keep, sizeof(keep));
    blank_flash();
}

// A sealed record that doesn't open never makes a secured board fall back to a plain one.
TEST(secured_board_no_fallback_to_plain_when_sealed_fails) {
    sos_config_t a, b;
    sealed_start();
    sample(&a, "Real");
    CHECK(config_save(&secured_store, &a));         // slot 0 sealed
    static uint8_t keep[CONFIG_SLOT_SIZE];
    memcpy(keep, flash.slot[0], sizeof(keep));
    blank_flash();
    plain_record("Planted");
    memcpy(flash.slot[1], flash.slot[0], CONFIG_SLOT_SIZE);
    memcpy(flash.slot[0], keep, sizeof(keep));
    flash.slot[0][16 + SEALER_NONCE_LEN + 5] ^= 1;  // the sealed record is damaged
    fix_sealed_crc(0);
    CHECK(!config_load(&secured_store, &b, NULL));
    CHECK(!strcmp(b.hostname, "sos"));
    CHECK(config_sealed_unopened(&secured_store));
}

// --- the owner secret ------------------------------------------------------------------------------

static void set_owner(sos_config_t *c, uint8_t first) {
    for (unsigned i = 0; i < sizeof(c->owner_secret); i++)
        c->owner_secret[i] = (uint8_t)(first ^ (i * 37));
}

static bool owner_is_zero(const sos_config_t *c) {
    for (unsigned i = 0; i < sizeof(c->owner_secret); i++)
        if (c->owner_secret[i])
            return false;
    return true;
}

// Is the owner secret (first, first+1, ...) readable anywhere in the fake flash?
static bool flash_has_owner(uint8_t first) {
    uint8_t want[CONFIG_OWNER_SECRET_LEN];
    for (unsigned i = 0; i < sizeof(want); i++)
        want[i] = (uint8_t)(first ^ (i * 37));
    for (unsigned slot = 0; slot < 2; slot++)
        for (size_t i = 0; i + sizeof(want) <= CONFIG_SLOT_SIZE; i++)
            if (!memcmp(flash.slot[slot] + i, want, sizeof(want)))
                return true;
    return false;
}

// Released layouts: the owner secret is the last field, after everything saved before it.
TEST(owner_secret_is_appended) {
    CHECK_EQ(offsetof(sos_config_t, owner_secret), 1644);
    CHECK_EQ(offsetof(sos_config_t, console_iterations), 1644 + CONFIG_OWNER_SECRET_LEN);
}

TEST(owner_secret_round_trips_sealed) {
    sos_config_t a, b;
    sealed_start();
    sample(&a, "HomeNet");
    set_owner(&a, 0xc0);
    CHECK(config_save(&sealed_store, &a));
    CHECK(!flash_has_owner(0xc0));                  // not readable in the sealed record
    CHECK(config_load(&sealed_store, &b, NULL));
    CHECK_MEM(a.owner_secret, b.owner_secret, sizeof(a.owner_secret));
}

// A plain save never writes it, even if the configuration in memory has one.
TEST(plain_save_never_writes_the_owner_secret) {
    sos_config_t a, b;
    blank_flash();
    sample(&a, "HomeNet");
    set_owner(&a, 0xc0);
    CHECK(config_save(&store, &a));
    CHECK(!flash_has_owner(0xc0));
    CHECK(config_load(&store, &b, NULL));
    CHECK(owner_is_zero(&b));
    CHECK(!strcmp(b.wifi_ssid, "HomeNet"));         // the rest is there
}

// A plain record that does hold bytes there (someone built one by hand) loads without them: an
// owner secret can't be planted, or carried over, in a plain record.
TEST(plain_record_cannot_carry_an_owner_secret) {
    sos_config_t a, b;
    sealed_start();
    sample(&a, "HomeNet");
    CHECK(config_save(&store, &a));                 // plain, slot 0
    for (unsigned i = 0; i < CONFIG_OWNER_SECRET_LEN; i++)
        flash.slot[0][16 + offsetof(sos_config_t, owner_secret) + i] = (uint8_t)(0xc0 ^ (i * 37));
    fix_crc(0);
    CHECK(flash_has_owner(0xc0));
    CHECK(config_load(&store, &b, NULL));           // the provisioning tool's view
    CHECK(owner_is_zero(&b));
    CHECK(config_load(&sealed_store, &b, NULL));    // the board's view
    CHECK(owner_is_zero(&b));
    CHECK(!strcmp(b.wifi_ssid, "HomeNet"));
    // The board gives it a new owner and seals it; the planted bytes are erased with the slot.
    set_owner(&b, 0x30);
    CHECK(config_save(&sealed_store, &b));
    CHECK(!flash_has_owner(0xc0));
    CHECK(config_load(&sealed_store, &a, NULL));
    CHECK_EQ(a.owner_secret[0], 0x30);
}

// Records from before the owner secret, plain or sealed, load with none.
TEST(records_from_before_the_owner_secret_load_with_none) {
    sos_config_t a, b;
    sealed_start();
    sample(&a, "HomeNet");
    strcpy(a.wifi_country, "US");
    CHECK(config_save(&store, &a));
    set_length(0, 1644);                            // plain, as the release before it wrote
    CHECK(config_load(&sealed_store, &b, NULL));
    CHECK(owner_is_zero(&b));
    CHECK(!strcmp(b.wifi_country, "US"));

    // Sealed with the shorter length, as the first sealing firmware wrote it: built by hand
    // with the same sealer (header, then nonce, ciphertext and tag over 1644 bytes).
    sealed_start();
    uint8_t *r = flash.slot[0];
    memset(r, 0xff, CONFIG_SLOT_SIZE);
    static const uint8_t header[12] = { 'S', 'O', 'S', 'C', 2, 0, 0x6c, 0x06, 7, 0, 0, 0 };
    memcpy(r, header, sizeof(header));
    CHECK(fake_seal(&fake, r, 12, (const uint8_t *)&a, 1644, r + 16));
    fix_sealed_crc(0);
    uint32_t seq = 0;
    memset(&b, 0x55, sizeof(b));
    CHECK(config_load(&sealed_store, &b, &seq));
    CHECK_EQ(seq, 7);
    CHECK(owner_is_zero(&b));
    CHECK(!strcmp(b.wifi_ssid, "HomeNet"));
    CHECK(!strcmp(b.wifi_country, "US"));
}

// --- the console password ----------------------------------------------------------------------------

static void set_verifier(sos_config_t *c) {
    c->console_iterations = 4000;
    memset(c->console_salt, 0xa1, sizeof(c->console_salt));
    memset(c->console_verifier, 0xb2, sizeof(c->console_verifier));
}

static bool has_verifier(const sos_config_t *c) {
    return c->console_iterations != 0 || c->console_salt[0] != 0 || c->console_verifier[0] != 0;
}

TEST(console_password_fields_are_appended) {
    CHECK_EQ(offsetof(sos_config_t, console_iterations), 1676);
    CHECK_EQ(offsetof(sos_config_t, console_salt), 1680);
    CHECK_EQ(offsetof(sos_config_t, console_verifier), 1696);
    CHECK_EQ(offsetof(sos_config_t, console_password), 1728);
    CHECK_EQ(offsetof(sos_config_t, wifi_extra), 1796);
}

TEST(console_password_checks) {
    CHECK(config_check_console_password("12345678") == NULL);
    CHECK(config_check_console_password("correct horse battery staple!") == NULL);
    CHECK(config_check_console_password("1234567") != NULL);                // too short
    CHECK(config_check_console_password("") != NULL);
    char longest[CONFIG_CONSOLE_PW_MAX + 2];
    memset(longest, 'a', sizeof(longest));
    longest[CONFIG_CONSOLE_PW_MAX] = '\0';
    CHECK(config_check_console_password(longest) == NULL);
    longest[CONFIG_CONSOLE_PW_MAX] = 'a';
    longest[CONFIG_CONSOLE_PW_MAX + 1] = '\0';
    CHECK(config_check_console_password(longest) != NULL);
    CHECK(config_check_console_password("tab\tinside") != NULL);            // not printable
    CHECK(config_check_console_password("caf\xc3\xa9 au lait") != NULL);    // not ASCII
}

// The verifier lives only in sealed records; the password to set only in plain ones.
TEST(verifier_round_trips_sealed_and_the_password_is_not_stored) {
    sos_config_t a, b;
    sealed_start();
    sample(&a, "HomeNet");
    set_verifier(&a);
    strcpy(a.console_password, "open sesame 123");      // still in memory when saving
    CHECK(config_save(&sealed_store, &a));
    CHECK(config_load(&sealed_store, &b, NULL));
    CHECK_EQ(b.console_iterations, 4000);
    CHECK_MEM(b.console_salt, a.console_salt, sizeof(a.console_salt));
    CHECK_MEM(b.console_verifier, a.console_verifier, sizeof(a.console_verifier));
    CHECK_EQ(b.console_password[0], '\0');              // never stored sealed
    // Not there under the seal either: opened by hand, the field is zero.
    static sos_config_t by_hand;
    CHECK(fake_open(&fake, flash.slot[0], 12, flash.slot[0] + 16,
                    sizeof(sos_config_t) + SEALER_OVERHEAD, (uint8_t *)&by_hand));
    for (unsigned i = 0; i < sizeof(by_hand.console_password); i++)
        CHECK_EQ(by_hand.console_password[i], 0);
}

TEST(plain_record_carries_the_password_but_never_a_verifier) {
    sos_config_t a, b;
    sealed_start();
    sample(&a, "HomeNet");
    set_verifier(&a);
    strcpy(a.console_password, "open sesame 123");
    CHECK(config_save(&store, &a));                     // the provisioning tool's save
    CHECK(config_load(&store, &b, NULL));
    CHECK(!has_verifier(&b));                           // not written
    CHECK(!strcmp(b.console_password, "open sesame 123"));
    // Planted verifier bytes in a plain record are dropped on load.
    uint32_t it = 4000;
    memcpy(flash.slot[0] + 16 + offsetof(sos_config_t, console_iterations), &it, 4);
    memset(flash.slot[0] + 16 + offsetof(sos_config_t, console_verifier), 0xb2, 32);
    fix_crc(0);
    CHECK(config_load(&sealed_store, &b, NULL));
    CHECK(!has_verifier(&b));
    CHECK(!strcmp(b.console_password, "open sesame 123"));
    // Sealing it (after the board has made the verifier) leaves no password in flash.
    set_verifier(&b);
    CHECK(config_save(&sealed_store, &b));
    CHECK(!slot_contains(0, "open sesame") && !slot_contains(1, "open sesame"));
}

TEST(bad_console_password_fields_are_rejected) {
    sos_config_t a, b;
    blank_flash();
    sample(&a, "HomeNet");
    strcpy(a.console_password, "short");                // a plain record with a bad password
    CHECK(!config_save(&store, &a));
    CHECK(!config_load(&store, &b, NULL));
    sealed_start();
    sample(&a, "HomeNet");
    set_verifier(&a);
    a.console_iterations = 5;                           // a count no verifier is made with
    CHECK(!config_save(&sealed_store, &a));
    a.console_iterations = CONFIG_CONSOLE_ITERATIONS_MAX + 1;
    CHECK(!config_save(&sealed_store, &a));
    a.console_iterations = CONFIG_CONSOLE_ITERATIONS_MIN;
    CHECK(config_save(&sealed_store, &a));
}

// Records from before the console password load with none set.
TEST(records_from_before_the_console_password_load_with_none) {
    sos_config_t a, b;
    sealed_start();
    sample(&a, "HomeNet");
    set_owner(&a, 0x33);
    uint8_t *r = flash.slot[0];
    memset(r, 0xff, CONFIG_SLOT_SIZE);
    static const uint8_t header[12] = { 'S', 'O', 'S', 'C', 2, 0, 0x8c, 0x06, 3, 0, 0, 0 };  // 1676
    memcpy(r, header, sizeof(header));
    CHECK(fake_seal(&fake, r, 12, (const uint8_t *)&a, 1676, r + 16));
    fix_sealed_crc(0);
    memset(&b, 0x55, sizeof(b));
    CHECK(config_load(&sealed_store, &b, NULL));
    CHECK(!has_verifier(&b));
    CHECK_EQ(b.console_password[0], '\0');
    CHECK_MEM(b.owner_secret, a.owner_secret, sizeof(a.owner_secret));
    CHECK(!strcmp(b.wifi_ssid, "HomeNet"));
}

// --- the glitch detector setting ---------------------------------------------------------------------

// It takes a byte that was padding: the record's size doesn't change, records saved before
// load as "not chosen" (the default: armed at the highest sensitivity), and a value out of
// range is that too, not an invalid record.
TEST(glitch_detector_setting) {
    sos_config_t a, b;
    CHECK_EQ(offsetof(sos_config_t, glitch_detector), 1793);
    CHECK_EQ(offsetof(sos_config_t, wifi_extra), 1796);     // the length of those records
    sealed_start();
    sample(&a, "HomeNet");
    CHECK_EQ(a.glitch_detector, CONFIG_GLITCH_DETECTOR_DEFAULT);
    CHECK_EQ(config_glitch_mode(CONFIG_GLITCH_DETECTOR_DEFAULT), 4);    // armed, sensitivity 3
    CHECK_EQ(config_glitch_mode(CONFIG_GLITCH_DETECTOR_OFF), 0);
    for (unsigned v = 1; v <= 8; v++)
        CHECK_EQ(config_glitch_mode(v), v);
    CHECK_EQ(config_glitch_mode(200), 4);
    for (unsigned v = 0; v <= CONFIG_GLITCH_DETECTOR_MAX; v++) {
        a.glitch_detector = (uint8_t)v;
        CHECK(config_save(&sealed_store, &a));
        CHECK(config_load(&sealed_store, &b, NULL));
        CHECK_EQ(b.glitch_detector, v);
    }
    a.glitch_detector = CONFIG_GLITCH_DETECTOR_MAX + 1; // can only be a leftover
    CHECK(config_save(&sealed_store, &a));
    CHECK(config_load(&sealed_store, &b, NULL));
    CHECK_EQ(b.glitch_detector, CONFIG_GLITCH_DETECTOR_DEFAULT);
    CHECK(!strcmp(b.wifi_ssid, "HomeNet"));
    blank_flash();                                      // a plain record carries it too
    a.glitch_detector = 2;
    CHECK(config_save(&store, &a));
    CHECK(config_load(&store, &b, NULL));
    CHECK_EQ(b.glitch_detector, 2);
}

// --- the chip temperature's calibration ----------------------------------------------------------

// It takes the two bytes that were padding before wifi_extra: the record's size doesn't change,
// records saved before load as uncalibrated, and a value out of range is that too.
TEST(temp_offset_setting) {
    sos_config_t a, b;
    CHECK_EQ(offsetof(sos_config_t, temp_offset), 1794);
    CHECK_EQ(offsetof(sos_config_t, wifi_extra), 1796);     // still where it was
    CHECK_EQ(sizeof(sos_config_t), 2188);
    sealed_start();
    sample(&a, "HomeNet");
    CHECK_EQ(a.temp_offset, 0);
    const int16_t values[] = {0, 18, -18, CONFIG_TEMP_OFFSET_MAX, -CONFIG_TEMP_OFFSET_MAX};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        a.temp_offset = values[i];
        CHECK(config_save(&sealed_store, &a));
        CHECK(config_load(&sealed_store, &b, NULL));
        CHECK_EQ(b.temp_offset, values[i]);
    }
    a.temp_offset = CONFIG_TEMP_OFFSET_MAX + 1;         // can only be a leftover
    CHECK(config_save(&sealed_store, &a));
    CHECK(config_load(&sealed_store, &b, NULL));
    CHECK_EQ(b.temp_offset, 0);
    a.temp_offset = -32768;
    CHECK(config_save(&sealed_store, &a));
    CHECK(config_load(&sealed_store, &b, NULL));
    CHECK_EQ(b.temp_offset, 0);
    CHECK(!strcmp(b.wifi_ssid, "HomeNet"));
    blank_flash();                                      // a plain record carries it too
    a.temp_offset = -7;
    CHECK(config_save(&store, &a));
    CHECK(config_load(&store, &b, NULL));
    CHECK_EQ(b.temp_offset, -7);
}

// --- saved Wi-Fi networks ----------------------------------------------------------------------------

static void check_net(const sos_config_t *c, unsigned i, const char *ssid, const char *password) {
    config_wifi_net_t n;
    CHECK(config_wifi_get(c, i, &n));
    CHECK(!strcmp(n.ssid, ssid));
    CHECK(!strcmp(n.password, password));
}

TEST(wifi_extra_is_appended) {
    CHECK_EQ(offsetof(sos_config_t, wifi_extra), 1796);
    CHECK_EQ(sizeof(config_wifi_net_t), 97);
    CHECK_EQ(sizeof(sos_config_t), 2188);
}

TEST(wifi_list_add_remove_prefer) {
    sos_config_t c;
    config_defaults(&c);
    CHECK_EQ(config_wifi_count(&c), 0);
    config_wifi_net_t n;
    CHECK(!config_wifi_get(&c, 0, &n));
    CHECK(config_wifi_add(&c, "home", "correct horse") == NULL);    // first: into wifi_ssid
    CHECK(!strcmp(c.wifi_ssid, "home"));
    CHECK(config_wifi_add(&c, "phone", "") == NULL);                // open network
    CHECK(config_wifi_add(&c, "work", "battery staple") == NULL);
    CHECK_EQ(config_wifi_count(&c), 3);
    check_net(&c, 1, "phone", "");
    check_net(&c, 2, "work", "battery staple");
    CHECK_EQ(config_wifi_find(&c, "work"), 2);
    CHECK_EQ(config_wifi_find(&c, "nope"), -1);
    CHECK(config_wifi_add(&c, "work", "") != NULL);                 // already saved
    CHECK(config_wifi_add(&c, "", "") != NULL);
    CHECK(config_wifi_add(&c, "x", "short") != NULL);
    CHECK_EQ(config_wifi_count(&c), 3);

    CHECK(config_wifi_prefer(&c, 2));                               // work, home, phone
    check_net(&c, 0, "work", "battery staple");
    check_net(&c, 1, "home", "correct horse");
    check_net(&c, 2, "phone", "");
    CHECK(!config_wifi_prefer(&c, 3));

    CHECK(config_wifi_remove(&c, 0));                               // home, phone
    CHECK_EQ(config_wifi_count(&c), 2);
    check_net(&c, 0, "home", "correct horse");
    check_net(&c, 1, "phone", "");
    CHECK_EQ(c.wifi_extra[1].ssid[0], 0);                           // nothing left behind
    CHECK(!config_wifi_remove(&c, 2));
    CHECK(config_wifi_remove(&c, 1));
    CHECK(config_wifi_remove(&c, 0));
    CHECK_EQ(config_wifi_count(&c), 0);
    CHECK_EQ(c.wifi_ssid[0], 0);
    CHECK_EQ(c.wifi_password[0], 0);
}

TEST(wifi_list_capacity) {
    sos_config_t c;
    config_defaults(&c);
    char name[8];
    for (unsigned i = 0; i < CONFIG_WIFI_NETS; i++) {
        snprintf(name, sizeof(name), "net%u", i);
        CHECK(config_wifi_add(&c, name, "") == NULL);
    }
    CHECK_EQ(config_wifi_count(&c), CONFIG_WIFI_NETS);
    CHECK(config_wifi_add(&c, "one more", "") != NULL);
    CHECK(config_wifi_prefer(&c, CONFIG_WIFI_NETS - 1));
    check_net(&c, 0, "net4", "");
    check_net(&c, CONFIG_WIFI_NETS - 1, "net3", "");
}

TEST(wifi_set_first_replaces_and_drops_a_duplicate) {
    sos_config_t c;
    config_defaults(&c);
    CHECK(config_wifi_set_first(&c, "home", "") == NULL);           // as the only one
    CHECK_EQ(config_wifi_count(&c), 1);
    CHECK(config_wifi_add(&c, "phone", "") == NULL);
    CHECK(config_wifi_add(&c, "work", "") == NULL);
    CHECK(config_wifi_set_first(&c, "cafe", "espresso!") == NULL);  // home is replaced
    CHECK_EQ(config_wifi_count(&c), 3);
    check_net(&c, 0, "cafe", "espresso!");
    CHECK(config_wifi_set_first(&c, "work", "new password") == NULL);   // moves up, once
    CHECK_EQ(config_wifi_count(&c), 2);
    check_net(&c, 0, "work", "new password");
    check_net(&c, 1, "phone", "");
    CHECK(config_wifi_set_first(&c, "x", "short") != NULL);
    check_net(&c, 0, "work", "new password");
}

TEST(wifi_networks_round_trip_sealed_and_plain) {
    sos_config_t a, b;
    sealed_start();
    sample(&a, "HomeNet");
    CHECK(config_wifi_add(&a, "Hotspot", "hotspot pass") == NULL);
    CHECK(config_save(&sealed_store, &a));
    CHECK(!slot_contains(0, "hotspot pass") && !slot_contains(1, "hotspot pass"));
    CHECK(config_load(&sealed_store, &b, NULL));
    CHECK_EQ(config_wifi_count(&b), 2);
    check_net(&b, 1, "Hotspot", "hotspot pass");
    blank_flash();
    CHECK(config_save(&store, &a));                         // the provisioning tool's save
    CHECK(config_load(&store, &b, NULL));
    check_net(&b, 1, "Hotspot", "hotspot pass");
}

// Records from before wifi_extra (1796 bytes) load with only the first network.
TEST(records_from_before_wifi_extra_load_with_one_network) {
    sos_config_t a, b;
    blank_flash();
    sample(&a, "HomeNet");
    CHECK(config_wifi_add(&a, "Hotspot", "") == NULL);
    CHECK(config_save(&store, &a));
    memset(flash.slot[0] + 16 + 1796, 0xff, CONFIG_SLOT_SIZE - 16 - 1796);
    set_length(0, 1796);
    CHECK(config_load(&store, &b, NULL));
    CHECK_EQ(config_wifi_count(&b), 1);
    CHECK(!strcmp(b.wifi_ssid, "HomeNet"));
    CHECK_EQ(b.wifi_extra[0].ssid[0], 0);
}

// Records from before the log rotation settings (2184 bytes) load with the defaults (0).
TEST(records_from_before_log_rotation_load_with_defaults) {
    sos_config_t a, b;
    CHECK_EQ(offsetof(sos_config_t, log_rotate_days), 2184);
    blank_flash();
    sample(&a, "HomeNet");
    a.log_rotate_days = 3;
    a.log_rotate_minute = 90;
    CHECK(config_save(&store, &a));
    CHECK(config_load(&store, &b, NULL));
    CHECK_EQ(b.log_rotate_days, 3);
    CHECK_EQ(b.log_rotate_minute, 90);
    memset(flash.slot[0] + 16 + 2184, 0xff, CONFIG_SLOT_SIZE - 16 - 2184);
    set_length(0, 2184);
    CHECK(config_load(&store, &b, NULL));
    CHECK(!strcmp(b.wifi_ssid, "HomeNet"));
    CHECK_EQ(b.log_rotate_days, 0);
    CHECK_EQ(b.log_rotate_minute, 0);
}

TEST(bad_log_rotation_is_rejected) {
    sos_config_t a;
    blank_flash();
    sample(&a, "HomeNet");
    a.log_rotate_days = CONFIG_LOG_ROTATE_DAYS_MAX;
    a.log_rotate_minute = CONFIG_LOG_ROTATE_MINUTE_MAX;
    CHECK(config_save(&store, &a));
    a.log_rotate_days = CONFIG_LOG_ROTATE_DAYS_MAX + 1;
    CHECK(!config_save(&store, &a));
    a.log_rotate_days = 1;
    a.log_rotate_minute = CONFIG_LOG_ROTATE_MINUTE_MAX + 1;
    CHECK(!config_save(&store, &a));
}

// Contents a record can't have: an entry after an unused one, entries without a first
// network, a password without a network, an invalid passphrase, an unterminated string.
TEST(bad_wifi_extra_is_rejected) {
    sos_config_t a, b;
    blank_flash();
    sample(&a, "HomeNet");
    strcpy(a.wifi_extra[1].ssid, "gap");
    CHECK(!config_save(&store, &a));
    sample(&a, "");
    strcpy(a.wifi_extra[0].ssid, "orphan");
    CHECK(!config_save(&store, &a));
    sample(&a, "HomeNet");
    strcpy(a.wifi_extra[0].password, "no network here");
    CHECK(!config_save(&store, &a));
    sample(&a, "HomeNet");
    strcpy(a.wifi_extra[0].ssid, "x");
    strcpy(a.wifi_extra[0].password, "short");
    CHECK(!config_save(&store, &a));
    sample(&a, "HomeNet");
    memset(a.wifi_extra[0].ssid, 'n', sizeof(a.wifi_extra[0].ssid));
    CHECK(!config_save(&store, &a));
    sample(&a, "HomeNet");
    CHECK(config_wifi_add(&a, "fine", "") == NULL);
    CHECK(config_save(&store, &a));
    CHECK(config_load(&store, &b, NULL));
}

TEST_MAIN(
    RUN(blank_flash_loads_defaults);
    RUN(save_and_load_round_trip);
    RUN(saves_alternate_slots);
    RUN(torn_write_keeps_previous_version);
    RUN(failed_write_keeps_previous_version);
    RUN(corrupt_newest_record_falls_back);
    RUN(both_corrupt_loads_defaults);
    RUN(out_of_range_contents_rejected_even_with_valid_crc);
    RUN(key_count_beyond_array_rejected);
    RUN(new_fields_round_trip);
    RUN(record_from_older_firmware_loads);
    RUN(record_length_out_of_range_rejected);
    RUN(record_from_second_release_loads);
    RUN(record_from_third_release_loads);
    RUN(record_from_fourth_release_loads);
    RUN(wifi_country_checked);
    RUN(rtc_offset_in_the_old_padding_byte);
    RUN(wifi_powersave_out_of_range_rejected);
    RUN(sequence_wraps_around);
    RUN(erase_resets_to_defaults);
    RUN(ssid_rules);
    RUN(password_rules);
    RUN(hostname_rules);
    RUN(ntp_server_rules);
    RUN(user_rules);
    RUN(base64_known_values);
    RUN(base64_rejects_malformed);
    RUN(parse_ed25519);
    RUN(parse_ecdsa);
    RUN(parse_without_comment_and_with_extra_spaces);
    RUN(parse_rejects_mismatched_embedded_type);
    RUN(long_comment_is_truncated);
    RUN(parse_rejects_bad_keys);
    RUN(key_list_add_find_remove);
    RUN(remove_keeps_order);
    RUN(key_list_capacity);
    RUN(crc32_known_value);
    RUN(glitch_detector_setting);
    RUN(temp_offset_setting);
    RUN(console_password_fields_are_appended);
    RUN(console_password_checks);
    RUN(verifier_round_trips_sealed_and_the_password_is_not_stored);
    RUN(plain_record_carries_the_password_but_never_a_verifier);
    RUN(bad_console_password_fields_are_rejected);
    RUN(records_from_before_the_console_password_load_with_none);
    RUN(owner_secret_is_appended);
    RUN(owner_secret_round_trips_sealed);
    RUN(plain_save_never_writes_the_owner_secret);
    RUN(plain_record_cannot_carry_an_owner_secret);
    RUN(records_from_before_the_owner_secret_load_with_none);
    RUN(sealed_save_and_load_round_trip);
    RUN(sealed_record_layout);
    RUN(sealed_saves_alternate_slots_with_fresh_nonces);
    RUN(sealed_torn_write_keeps_previous_version);
    RUN(save_fails_when_sealing_fails);
    RUN(changed_sealed_record_does_not_load);
    RUN(sealed_rollback_by_sequence_number_fails);
    RUN(sealed_under_another_key_does_not_load);
    RUN(sealed_contents_out_of_range_rejected);
    RUN(store_without_sealer_ignores_sealed_records);
    RUN(plain_record_loads_then_is_sealed_and_erased);
    RUN(old_plain_record_is_sealed_with_its_fields);
    RUN(an_interrupted_sealing_is_done_again);
    RUN(plain_record_saved_after_a_downgrade_wins);
    RUN(secured_board_ignores_a_plain_record_saved_after_a_downgrade);
    RUN(scrub_keeps_a_plain_record_that_is_the_only_copy);
    RUN(failed_erase_of_the_plain_copy_is_reported);
    RUN(scrub_erases_a_damaged_plain_record);
    RUN(secured_board_does_not_load_a_plain_record);
    RUN(secured_board_plain_record_cannot_hide_the_sealed_one);
    RUN(secured_board_no_fallback_to_plain_when_sealed_fails);
    RUN(wifi_extra_is_appended);
    RUN(records_from_before_log_rotation_load_with_defaults);
    RUN(bad_log_rotation_is_rejected);
    RUN(wifi_list_add_remove_prefer);
    RUN(wifi_list_capacity);
    RUN(wifi_set_first_replaces_and_drops_a_duplicate);
    RUN(wifi_networks_round_trip_sealed_and_plain);
    RUN(records_from_before_wifi_extra_load_with_one_network);
    RUN(bad_wifi_extra_is_rejected);
)
