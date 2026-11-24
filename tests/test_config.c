// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/config.c: A/B record storage, validation, key parsing and the key list.

#include <stdint.h>

#include "config.h"
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
static config_store_t store = { flash_read, flash_write, &flash };

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
)
