// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/bootkey.c: finding the key an image is signed with, the votes OTP
// flag rows are read by, and the comparison with the boot key slots.
//
// The images here are built by hand from the block format in the RP2350 datasheet (section
// 5.9). That the parser also finds the key in an image picotool really signed was checked once
// by hand against `picotool info -a` and the signing key (see the pull request); a fixture of
// that size isn't kept in the tests.

#include <string.h>

#include "core/bootkey.h"
#include "test.h"

static uint8_t img[8192];

static void put32(size_t off, uint32_t v) {
    img[off] = (uint8_t)v;
    img[off + 1] = (uint8_t)(v >> 8);
    img[off + 2] = (uint8_t)(v >> 16);
    img[off + 3] = (uint8_t)(v >> 24);
}

// Write a block at `off`: optionally an image definition item first, optionally a signature
// whose key bytes are all `key`, then the last item, the link and the end marker. Returns the
// block's length.
static size_t block(size_t off, bool image_def, int key, int32_t link) {
    size_t pos = off;
    put32(pos, 0xffffded3u);
    pos += 4;
    if (image_def) {
        put32(pos, 0x42u | 1u << 8 | 0x1021u << 16);    // IMAGE_DEF, one word
        pos += 4;
    } else {
        put32(pos, 0x0au | 2u << 8);                    // some other item, two words
        put32(pos + 4, 0);
        pos += 8;
    }
    if (key >= 0) {
        put32(pos, 0x09u | 0x21u << 8 | 0x01u << 24);   // SIGNATURE, secp256k1
        memset(img + pos + 4, key, 64);
        memset(img + pos + 68, 0xee, 64);
        pos += 4 * 0x21;
    }
    size_t words = (pos - off - 4) / 4;
    put32(pos, 0xffu | (uint32_t)words << 8);
    put32(pos + 4, (uint32_t)link);
    put32(pos + 8, 0xab123579u);
    return pos + 12 - off;
}

static void blank(void) { memset(img, 0x5a, sizeof(img)); }

TEST(unsigned_image) {
    const uint8_t *key = (const uint8_t *)1;
    blank();
    block(0x100, true, -1, 0);
    CHECK_EQ(bootkey_image_pubkey(img, sizeof(img), &key), BOOTKEY_IMAGE_UNSIGNED);
    CHECK(key == NULL);
}

// As picotool leaves a signed image: the first block, unsigned, links to a second at the end,
// which carries the signature and links back.
TEST(signed_image_second_block) {
    const uint8_t *key;
    blank();
    block(0x100, true, -1, 0x1000);
    block(0x1100, true, 0x33, -0x1000);
    CHECK_EQ(bootkey_image_pubkey(img, sizeof(img), &key), BOOTKEY_IMAGE_SIGNED);
    CHECK(key != NULL && key > img + 0x1100 && key[0] == 0x33 && key[63] == 0x33);
}

TEST(signed_single_block) {
    const uint8_t *key;
    blank();
    block(0, true, 0x44, 0);
    CHECK_EQ(bootkey_image_pubkey(img, sizeof(img), &key), BOOTKEY_IMAGE_SIGNED);
    CHECK(key[0] == 0x44);
}

// The boot ROM takes the LAST image definition in the loop. A signed one followed by an
// unsigned one is therefore an unsigned image.
TEST(last_image_def_wins) {
    const uint8_t *key;
    blank();
    block(0x100, true, 0x11, 0x400);
    block(0x500, true, 0x22, 0x400);
    block(0x900, true, -1, -0x800);
    CHECK_EQ(bootkey_image_pubkey(img, sizeof(img), &key), BOOTKEY_IMAGE_UNSIGNED);
    blank();
    block(0x100, true, 0x11, 0x400);
    block(0x500, true, 0x22, -0x400);
    CHECK_EQ(bootkey_image_pubkey(img, sizeof(img), &key), BOOTKEY_IMAGE_SIGNED);
    CHECK(key[0] == 0x22);
}

// A signature in a block that isn't an image definition (a partition table's, say) isn't the
// image's.
TEST(signature_elsewhere_is_not_the_images) {
    const uint8_t *key;
    blank();
    block(0x100, true, -1, 0x400);
    block(0x500, false, 0x77, -0x400);
    CHECK_EQ(bootkey_image_pubkey(img, sizeof(img), &key), BOOTKEY_IMAGE_UNSIGNED);
}

TEST(nothing_recognisable) {
    const uint8_t *key;
    blank();
    CHECK_EQ(bootkey_image_pubkey(img, sizeof(img), &key), BOOTKEY_IMAGE_NO_BLOCKS);
    memset(img, 0xff, sizeof(img));                     // erased flash
    CHECK_EQ(bootkey_image_pubkey(img, sizeof(img), &key), BOOTKEY_IMAGE_NO_BLOCKS);
    CHECK_EQ(bootkey_image_pubkey(img, 0, &key), BOOTKEY_IMAGE_NO_BLOCKS);
    CHECK_EQ(bootkey_image_pubkey(img, 8, &key), BOOTKEY_IMAGE_NO_BLOCKS);
    blank();
    block(0x1000, true, 0x11, 0);                       // first block too far in
    CHECK_EQ(bootkey_image_pubkey(img, sizeof(img), &key), BOOTKEY_IMAGE_NO_BLOCKS);
    blank();
    block(0x100, false, 0x11, 0);                       // a closed loop with no image definition
    CHECK_EQ(bootkey_image_pubkey(img, sizeof(img), &key), BOOTKEY_IMAGE_NO_BLOCKS);
}

TEST(damaged_blocks_are_not_believed) {
    const uint8_t *key;
    blank();
    size_t n = block(0x100, true, 0x11, 0);
    put32(0x100 + n - 4, 0xab123578u);                  // wrong end marker
    CHECK_EQ(bootkey_image_pubkey(img, sizeof(img), &key), BOOTKEY_IMAGE_NO_BLOCKS);
    blank();
    n = block(0x100, true, 0x11, 0);
    img[0x100 + n - 11] ^= 1;                           // the last item's word count
    CHECK_EQ(bootkey_image_pubkey(img, sizeof(img), &key), BOOTKEY_IMAGE_NO_BLOCKS);
    blank();
    block(0x100, true, 0x11, 0x400);                    // links to nothing: the loop isn't closed
    CHECK_EQ(bootkey_image_pubkey(img, sizeof(img), &key), BOOTKEY_IMAGE_NO_BLOCKS);
    blank();
    block(0x100, true, 0x11, -0x200);                   // links before the image
    CHECK_EQ(bootkey_image_pubkey(img, sizeof(img), &key), BOOTKEY_IMAGE_NO_BLOCKS);
    blank();
    block(0x100, true, -1, 0x1000);                     // a loop that never returns to the first
    block(0x1100, true, 0x11, 0);                       // (and is too far in to be a first block)
    CHECK_EQ(bootkey_image_pubkey(img, sizeof(img), &key), BOOTKEY_IMAGE_NO_BLOCKS);
}

// Nothing past `len` is read: a block cut off by the end of the buffer isn't a block. (The
// sanitizers would catch a read beyond the array.)
TEST(stays_inside_the_buffer) {
    const uint8_t *key;
    for (size_t cut = 0; cut < 160; cut += 4) {
        blank();
        size_t n = block(sizeof(img) - 160, true, 0x11, 0);
        if (cut >= n)
            break;
        CHECK_EQ(bootkey_image_pubkey(img + sizeof(img) - 160, cut, &key), BOOTKEY_IMAGE_NO_BLOCKS);
    }
    blank();
    size_t n = block(0, true, 0x11, 0);
    CHECK_EQ(bootkey_image_pubkey(img, n, &key), BOOTKEY_IMAGE_SIGNED);
    CHECK_EQ(bootkey_image_pubkey(img, n - 4, &key), BOOTKEY_IMAGE_NO_BLOCKS);
    // An item whose size runs past the block limit.
    blank();
    put32(0, 0xffffded3u);
    put32(4, 0x42u | 0xffu << 8);
    CHECK_EQ(bootkey_image_pubkey(img, sizeof(img), &key), BOOTKEY_IMAGE_NO_BLOCKS);
    put32(4, 0x42u);                                    // an item of no words would never end
    CHECK_EQ(bootkey_image_pubkey(img, sizeof(img), &key), BOOTKEY_IMAGE_NO_BLOCKS);
}

TEST(votes) {
    uint32_t crit[8] = {1, 1, 1, 0, 0, 0, 0, 0};
    CHECK_EQ(bootkey_vote(crit, 8, 3), 1);              // three of eight is set
    crit[2] = 0;
    CHECK_EQ(bootkey_vote(crit, 8, 3), 0);              // two of eight is not
    uint32_t flags[3] = {0x0f, 0x03, 0x05};
    CHECK_EQ(bootkey_vote(flags, 3, 2), 0x07);          // each bit by two of three
    uint32_t all[3] = {0xffffff, 0xffffff, 0};
    CHECK_EQ(bootkey_vote(all, 3, 2), 0xffffff);
}

TEST(slot_hash_byte_order) {
    uint16_t rows[BOOTKEY_SLOT_ROWS];
    uint8_t hash[BOOTKEY_HASH_LEN];
    for (unsigned k = 0; k < BOOTKEY_SLOT_ROWS; k++)
        rows[k] = (uint16_t)((2 * k + 1) << 8 | 2 * k); // bytes 0, 1, 2, ... in order
    bootkey_slot_hash(rows, hash);
    for (unsigned i = 0; i < BOOTKEY_HASH_LEN; i++)
        CHECK_EQ(hash[i], i);
}

TEST(compare) {
    bootkey_otp_t otp;
    uint8_t h[BOOTKEY_HASH_LEN];
    int slot;
    memset(&otp, 0, sizeof(otp));
    memset(h, 0xa5, sizeof(h));
    CHECK_EQ(bootkey_compare(&otp, h, &slot), BOOTKEY_NO_MATCH);    // a blank board
    CHECK_EQ(slot, -1);
    for (unsigned s = 0; s < BOOTKEY_SLOTS; s++)
        CHECK(bootkey_slot_blank(&otp, s) && !bootkey_slot_usable(&otp, s));

    memset(otp.slot[1], 0xa5, BOOTKEY_HASH_LEN);                     // written, not marked
    CHECK(!bootkey_slot_blank(&otp, 1));
    CHECK_EQ(bootkey_compare(&otp, h, &slot), BOOTKEY_MATCH_NOT_VALID);
    CHECK_EQ(slot, 1);
    otp.valid = 1u << 0;                                            // another slot's mark
    CHECK_EQ(bootkey_compare(&otp, h, &slot), BOOTKEY_MATCH_NOT_VALID);
    otp.valid = 1u << 1;
    CHECK_EQ(bootkey_compare(&otp, h, &slot), BOOTKEY_MATCH);
    CHECK_EQ(slot, 1);
    CHECK(bootkey_slot_usable(&otp, 1));
    otp.invalid = 1u << 1;                                          // revoked
    CHECK_EQ(bootkey_compare(&otp, h, &slot), BOOTKEY_MATCH_INVALIDATED);
    CHECK(!bootkey_slot_usable(&otp, 1));

    memset(otp.slot[3], 0xa5, BOOTKEY_HASH_LEN);                     // the same key, usable, later
    otp.valid |= 1u << 3;
    CHECK_EQ(bootkey_compare(&otp, h, &slot), BOOTKEY_MATCH);
    CHECK_EQ(slot, 3);

    otp.slot[3][31] ^= 1;                                           // one bit off is no match
    otp.slot[1][0] ^= 0x80;
    CHECK_EQ(bootkey_compare(&otp, h, &slot), BOOTKEY_NO_MATCH);
    CHECK_EQ(slot, -1);
    CHECK(!bootkey_slot_usable(&otp, 4));
}

TEST_MAIN(
    RUN(unsigned_image);
    RUN(signed_image_second_block);
    RUN(signed_single_block);
    RUN(last_image_def_wins);
    RUN(signature_elsewhere_is_not_the_images);
    RUN(nothing_recognisable);
    RUN(damaged_blocks_are_not_believed);
    RUN(stays_inside_the_buffer);
    RUN(votes);
    RUN(slot_hash_byte_order);
    RUN(compare);
)
