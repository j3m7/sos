// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/files/sdcrypt.c (SD card encryption: header and sectors), with wolfCrypt
// built for the host, cross-checked against tools/host/sdcrypt.c (OpenSSL's libcrypto).

#define _GNU_SOURCE            // memmem
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "files/sdcrypt.h"
#include "sdcrypt_tool.h"       // tools/host/sdcrypt.c, built with SDCRYPT_TOOL_NO_MAIN
#include "test.h"

static uint8_t devkey[32], other_devkey[32], card_id[16], nonce[12], data_key[32];

static void fill(uint8_t *p, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)(seed + i * 37);
}

static void setup(void) {
    fill(devkey, sizeof(devkey), 1);
    fill(other_devkey, sizeof(other_devkey), 2);
    fill(card_id, sizeof(card_id), 3);
    fill(nonce, sizeof(nonce), 4);
    fill(data_key, sizeof(data_key), 5);
}

TEST(header_round_trip) {
    setup();
    uint8_t sector[512];
    CHECK(sdcrypt_header_build(sector, devkey, card_id, nonce, data_key));
    CHECK(sdcrypt_is_header(sector));
    CHECK(!memcmp(sector, "SOS-CRYPT-CARD\0\0", 16));
    // The data key isn't stored in the clear.
    CHECK(memmem(sector, sizeof(sector), data_key, sizeof(data_key)) == NULL);
    for (int i = 100; i < 512; i++)
        CHECK_EQ(sector[i], 0);
    sdcrypt_info_t info;
    CHECK_EQ(sdcrypt_header_open(sector, devkey, &info), SDCRYPT_OK);
    CHECK_MEM(info.data_key, data_key, 32);
    CHECK_MEM(info.card_id, card_id, 16);
    CHECK_EQ(info.offset, SDCRYPT_OFFSET);
    // The host tool reads the same header.
    uint32_t offset = 0;
    CHECK_EQ(sdcrypt_tool_header(sector, &offset), 0);
    CHECK_EQ(offset, SDCRYPT_OFFSET);
}

TEST(header_rejected) {
    setup();
    uint8_t good[512], s[512];
    sdcrypt_info_t info;
    sdcrypt_header_build(good, devkey, card_id, nonce, data_key);

    // Another device's key: refused, nothing leaked into info.
    CHECK_EQ(sdcrypt_header_open(good, other_devkey, &info), SDCRYPT_WRONG_KEY);
    uint8_t zero[sizeof(info)] = {0};
    CHECK_MEM(&info, zero, sizeof(info));

    // Not a header: a plain card's MBR, or zeros.
    memset(s, 0, sizeof(s));
    s[510] = 0x55; s[511] = 0xaa;
    CHECK(!sdcrypt_is_header(s));
    CHECK_EQ(sdcrypt_header_open(s, devkey, &info), SDCRYPT_PLAIN);

    // Unknown version, cipher, wrap or offset.
    static const int fields[] = { 16, 17, 18, 20 };
    for (unsigned f = 0; f < sizeof(fields) / sizeof(fields[0]); f++) {
        memcpy(s, good, sizeof(s));
        s[fields[f]] ^= 0x02;
        CHECK_EQ(sdcrypt_header_open(s, devkey, &info), SDCRYPT_UNSUPPORTED);
    }

    // Any change to the authenticated fields, nonce, wrapped key or tag fails the tag.
    for (int i = 21; i < 100; i++) {           // (21-23: the offset, unsupported)
        memcpy(s, good, sizeof(s));
        s[i] ^= 0x01;
        int r = sdcrypt_header_open(s, devkey, &info);
        CHECK(r == SDCRYPT_WRONG_KEY || r == SDCRYPT_UNSUPPORTED);
        if (i >= 24)
            CHECK_EQ(r, SDCRYPT_WRONG_KEY);
    }
}

// Which copy of the header a card is opened from (sdcard.c reads sector 0 and the backup).
TEST(card_open_rules) {
    setup();
    static uint8_t good[512], other[512], damaged[512], mbr[512], junk[512];
    sdcrypt_info_t info;
    bool from_backup;
    sdcrypt_header_build(good, devkey, card_id, nonce, data_key);
    sdcrypt_header_build(other, other_devkey, card_id, nonce, data_key);
    memcpy(damaged, good, 512);
    damaged[90] ^= 0x10;                        // a bit in the tag: magic still there
    memset(mbr, 0, 512);
    mbr[510] = 0x55; mbr[511] = 0xaa;
    fill(junk, 512, 77);

    // Sector 0 opens: the backup isn't needed.
    CHECK_EQ(sdcrypt_card_open(good, good, devkey, &info, &from_backup), SDCRYPT_OK);
    CHECK(!from_backup);
    CHECK_MEM(info.data_key, data_key, 32);
    CHECK_EQ(sdcrypt_card_open(good, NULL, devkey, &info, &from_backup), SDCRYPT_OK);

    // Sector 0 damaged but still a header: the backup opens the card (the review's case).
    CHECK_EQ(sdcrypt_card_open(damaged, good, devkey, &info, &from_backup), SDCRYPT_OK);
    CHECK(from_backup);
    CHECK_MEM(info.data_key, data_key, 32);
    // ...unsupported version in sector 0: same.
    memcpy(damaged, good, 512);
    damaged[16] = 9;
    CHECK_EQ(sdcrypt_card_open(damaged, good, devkey, &info, &from_backup), SDCRYPT_OK);
    CHECK(from_backup);
    // Sector 0 overwritten with junk (not a boot sector): the backup.
    CHECK_EQ(sdcrypt_card_open(junk, good, devkey, &info, &from_backup), SDCRYPT_OK);
    CHECK(from_backup);

    // Sector 0 a boot sector/MBR: plain, whatever the backup says (a PC formatted the card, or a
    // plain card with an old header copy).
    CHECK_EQ(sdcrypt_card_open(mbr, good, devkey, &info, &from_backup), SDCRYPT_PLAIN);
    CHECK(!from_backup);
    CHECK_EQ(sdcrypt_card_open(junk, mbr, devkey, &info, &from_backup), SDCRYPT_PLAIN);
    CHECK_EQ(sdcrypt_card_open(junk, NULL, devkey, &info, &from_backup), SDCRYPT_PLAIN);

    // Neither copy opens: sector 0's reason, info wiped.
    uint8_t zero[sizeof(info)] = {0};
    memcpy(damaged, good, 512);
    damaged[90] ^= 0x10;
    CHECK_EQ(sdcrypt_card_open(damaged, damaged, devkey, &info, &from_backup), SDCRYPT_WRONG_KEY);
    CHECK_MEM(&info, zero, sizeof(info));
    CHECK_EQ(sdcrypt_card_open(other, other, devkey, &info, &from_backup), SDCRYPT_WRONG_KEY);
    CHECK_EQ(sdcrypt_card_open(damaged, NULL, devkey, &info, &from_backup), SDCRYPT_WRONG_KEY);
    // Sector 0 junk, backup another device's: the backup's reason.
    CHECK_EQ(sdcrypt_card_open(junk, other, devkey, &info, &from_backup), SDCRYPT_WRONG_KEY);
    CHECK(!from_backup);

    // No device key: a header can't be opened, a plain card is still plain.
    CHECK_EQ(sdcrypt_card_open(good, good, NULL, &info, &from_backup), SDCRYPT_NO_DEVKEY);
    CHECK_EQ(sdcrypt_card_open(mbr, NULL, NULL, &info, &from_backup), SDCRYPT_PLAIN);
}

TEST(sectors_round_trip) {
    setup();
    static sdcrypt_t c;
    static uint8_t plain[8 * 512], enc[8 * 512], buf[8 * 512];
    fill(plain, sizeof(plain), 9);
    CHECK(!sdcrypt_encrypt(&c, enc, plain, 0, 1));     // not started
    CHECK(sdcrypt_start(&c, data_key));
    static const uint32_t starts[] = { 0, 1, 8191, 123456789, 0xfffffff8u };
    for (unsigned k = 0; k < sizeof(starts) / sizeof(starts[0]); k++) {
        CHECK(sdcrypt_encrypt(&c, enc, plain, starts[k], 8));
        CHECK(memcmp(enc, plain, 512) != 0);
        // Each sector differs even with the same plaintext (the tweak is the sector number).
        CHECK(memcmp(enc, enc + 512, 512) != 0 || memcmp(plain, plain + 512, 512) != 0);
        memcpy(buf, enc, sizeof(buf));
        CHECK(sdcrypt_decrypt(&c, buf, starts[k], 8));       // in place
        CHECK_MEM(buf, plain, sizeof(plain));
        // Decrypting at the wrong sector number gives garbage.
        memcpy(buf, enc, 512);
        CHECK(sdcrypt_decrypt(&c, buf, starts[k] + 1, 1));
        CHECK(memcmp(buf, plain, 512) != 0);
    }
    // Same plaintext in every sector: still different ciphertext per sector.
    memset(plain, 0, sizeof(plain));
    CHECK(sdcrypt_encrypt(&c, enc, plain, 100, 8));
    for (int i = 1; i < 8; i++)
        CHECK(memcmp(enc, enc + i * 512, 512) != 0);
    sdcrypt_stop(&c);
    CHECK(!c.ready);
    CHECK(!sdcrypt_decrypt(&c, buf, 0, 1));
}

TEST(matches_openssl) {
    setup();
    static sdcrypt_t c;
    static uint8_t plain[4 * 512], dev[4 * 512], ossl[4 * 512];
    fill(plain, sizeof(plain), 11);
    CHECK(sdcrypt_start(&c, data_key));
    static const uint64_t starts[] = { 0, 7, 8192, 4000000000ull };
    for (unsigned k = 0; k < sizeof(starts) / sizeof(starts[0]); k++) {
        // Device encrypts, OpenSSL (aes-128-xts, plain64 tweak) decrypts, and the other way.
        CHECK(sdcrypt_encrypt(&c, dev, plain, (uint32_t)starts[k], 4));
        CHECK_EQ(sdcrypt_tool_xts(1, data_key, starts[k], plain, ossl, 4), 0);
        CHECK_MEM(dev, ossl, sizeof(dev));
        CHECK_EQ(sdcrypt_tool_xts(0, data_key, starts[k], dev, ossl, 4), 0);
        CHECK_MEM(ossl, plain, sizeof(plain));
    }
    sdcrypt_stop(&c);
}

// FatFs hands the driver whatever buffer the caller gave f_read/f_write, at any alignment.
TEST(unaligned_buffers) {
    setup();
    static sdcrypt_t c;
    static uint8_t raw_plain[2 * 512 + 8], raw_enc[2 * 512 + 8], ref[2 * 512];
    CHECK(sdcrypt_start(&c, data_key));
    for (int off = 1; off < 8; off += 2) {
        uint8_t *p = raw_plain + off, *e = raw_enc + off;
        fill(p, 2 * 512, (uint8_t)off);
        CHECK(sdcrypt_encrypt(&c, e, p, 42, 2));
        CHECK_EQ(sdcrypt_tool_xts(1, data_key, 42, p, ref, 2), 0);
        CHECK_MEM(e, ref, sizeof(ref));
        CHECK(sdcrypt_decrypt(&c, e, 42, 2));
        CHECK_MEM(e, p, 2 * 512);
    }
    sdcrypt_stop(&c);
}

#ifdef SDCRYPT_TOOL_PATH
// The whole recovery path: an image of an encrypted card as the device lays it out (header,
// backup, volume from sector 8192), decrypted by the sdcrypt tool with a key file.
TEST(tool_decrypts_image) {
    setup();
    static sdcrypt_t c;
    static uint8_t sector[512], plain[512], out[512];
    const unsigned volume = 64;                 // sectors of volume in the test image
    FILE *f = fopen("sdcrypt-test.img", "wb");
    CHECK(f != NULL);
    if (!f)
        return;
    sdcrypt_header_build(sector, devkey, card_id, nonce, data_key);
    for (unsigned s = 0; s < SDCRYPT_OFFSET; s++) {
        static const uint8_t zero[512];
        fwrite(s == 0 || s == SDCRYPT_BACKUP ? sector : zero, 1, 512, f);
    }
    CHECK(sdcrypt_start(&c, data_key));
    for (unsigned s = 0; s < volume; s++) {
        fill(plain, 512, (uint8_t)s);
        sdcrypt_encrypt(&c, sector, plain, s, 1);
        fwrite(sector, 1, 512, f);
    }
    sdcrypt_stop(&c);
    fclose(f);
    f = fopen("sdcrypt-test.key", "w");
    for (int i = 0; i < 32; i++)
        fprintf(f, "%02x", data_key[i]);
    fprintf(f, "\n");
    fclose(f);

    CHECK_EQ(system(SDCRYPT_TOOL_PATH " decrypt sdcrypt-test.img sdcrypt-test.key "
                    "sdcrypt-test.out >/dev/null 2>&1"), 0);
    f = fopen("sdcrypt-test.out", "rb");
    CHECK(f != NULL);
    unsigned n = 0;
    while (f && fread(out, 1, 512, f) == 512) {
        fill(plain, 512, (uint8_t)n);
        CHECK_MEM(out, plain, 512);
        n++;
    }
    if (f)
        fclose(f);
    CHECK_EQ(n, volume);
    // A wrong-length key is refused.
    f = fopen("sdcrypt-test.key", "w");
    fprintf(f, "abcd\n");
    fclose(f);
    CHECK(system(SDCRYPT_TOOL_PATH " decrypt sdcrypt-test.img sdcrypt-test.key "
                 "sdcrypt-test.out >/dev/null 2>&1") != 0);
    remove("sdcrypt-test.img");
    remove("sdcrypt-test.key");
    remove("sdcrypt-test.out");
}
#endif

TEST(wipe) {
    uint8_t k[32];
    memset(k, 0xaa, sizeof(k));
    sdcrypt_wipe(k, sizeof(k));
    uint8_t zero[32] = {0};
    CHECK_MEM(k, zero, 32);
}

// The card key: HMAC-SHA256 keyed with the wrap base key, over the owner secret. The expected
// value was computed with Python's hmac/hashlib for base 40..5f and owner 80..9f.
TEST(wrap_key_known_answer) {
    uint8_t base[32], owner[32], key[32], other[32];
    static const uint8_t want[32] = {
        0xe0, 0x65, 0x67, 0x59, 0x4e, 0xfd, 0x01, 0x4c, 0x59, 0x14, 0x9b, 0x4c, 0xae, 0x2b, 0x95, 0xb1,
        0x02, 0x49, 0xc8, 0xf5, 0x0c, 0xac, 0xd6, 0x8a, 0xbd, 0x83, 0x71, 0xbb, 0xe0, 0x87, 0x9a, 0x95,
    };
    for (unsigned i = 0; i < 32; i++) {
        base[i] = (uint8_t)(0x40 + i);
        owner[i] = (uint8_t)(0x80 + i);
    }
    CHECK(sdcrypt_wrap_key(key, base, owner));
    CHECK_MEM(key, want, sizeof(want));
    // Another owner (the configuration was erased and set up again): another key. Another board
    // (another base): another key.
    owner[31] ^= 1;
    CHECK(sdcrypt_wrap_key(other, base, owner));
    CHECK(memcmp(key, other, 32) != 0);
    owner[31] ^= 1;
    base[0] ^= 1;
    CHECK(sdcrypt_wrap_key(other, base, owner));
    CHECK(memcmp(key, other, 32) != 0);
}

// A card is locked by a change of owner: a header built under one owner's card key doesn't
// open under the next owner's.
TEST(another_owner_does_not_open_the_card) {
    setup();
    uint8_t base[32], owner[32], key_a[32], key_b[32], s[512];
    sdcrypt_info_t info;
    memset(base, 0x11, sizeof(base));
    memset(owner, 0x22, sizeof(owner));
    CHECK(sdcrypt_wrap_key(key_a, base, owner));
    owner[0] ^= 0x80;
    CHECK(sdcrypt_wrap_key(key_b, base, owner));
    CHECK(sdcrypt_header_build(s, key_a, card_id, nonce, data_key));
    CHECK_EQ(sdcrypt_header_open(s, key_a, &info), SDCRYPT_OK);
    CHECK_EQ(sdcrypt_header_open(s, key_b, &info), SDCRYPT_WRONG_KEY);
}

// The header says which key wraps it: 2, the card key. A header with 1 (the device key that
// firmware before the owner secret kept in flash) is recognised and not opened, with or
// without a key, from sector 0 or the backup.
TEST(old_device_key_cards_are_not_opened) {
    setup();
    uint8_t s[512], junk[512];
    sdcrypt_info_t info;
    bool from_backup;
    CHECK(sdcrypt_header_build(s, devkey, card_id, nonce, data_key));
    CHECK_EQ(s[18], 2);
    s[18] = 1;
    CHECK_EQ(sdcrypt_header_open(s, devkey, &info), SDCRYPT_OLD_WRAP);
    uint8_t zero[sizeof(info)] = {0};
    CHECK_MEM(&info, zero, sizeof(info));
    CHECK_EQ(sdcrypt_card_open(s, s, devkey, &info, &from_backup), SDCRYPT_OLD_WRAP);
    CHECK_EQ(sdcrypt_card_open(s, s, NULL, &info, &from_backup), SDCRYPT_OLD_WRAP);
    memset(junk, 0xa5, sizeof(junk));
    CHECK_EQ(sdcrypt_card_open(junk, s, devkey, &info, &from_backup), SDCRYPT_OLD_WRAP);
    CHECK(!from_backup);
}

TEST_MAIN(
    RUN(wrap_key_known_answer);
    RUN(another_owner_does_not_open_the_card);
    RUN(old_device_key_cards_are_not_opened);
    RUN(header_round_trip);
    RUN(header_rejected);
    RUN(card_open_rules);
    RUN(sectors_round_trip);
    RUN(matches_openssl);
    RUN(unaligned_buffers);
#ifdef SDCRYPT_TOOL_PATH
    RUN(tool_decrypts_image);
#endif
    RUN(wipe);
)
