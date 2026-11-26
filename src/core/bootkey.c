// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See bootkey.h. The block format is the RP2350 datasheet's section 5.9; the constants are the
// Pico SDK's boot/picobin.h, repeated here so that this file builds on the host.

#include "core/bootkey.h"

#include <string.h>

#define MARKER_START   0xffffded3u
#define MARKER_END     0xab123579u
#define ITEM_IMAGE_DEF 0x42             // PICOBIN_BLOCK_ITEM_1BS_IMAGE_TYPE
#define ITEM_SIGNATURE 0x09
#define ITEM_LAST      0xff
#define SIG_WORDS      0x21             // header word, 16 words of key, 16 of signature
#define SIG_SECP256K1  0x01
#define MAX_BLOCK      0x280u           // bytes: the boot ROM ignores larger blocks
#define FIRST_BLOCK_WITHIN 4096u
#define MAX_BLOCKS     16               // in a loop; real images have one to three

static uint32_t word(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// One block at `off`: is it well formed, where does it link to, is it an image definition, and
// where is its signature's key. Returns false if there is no valid block there.
typedef struct {
    int32_t link;
    bool image_def;
    const uint8_t *pubkey;
} block_t;

static bool block_at(const uint8_t *image, size_t len, size_t off, block_t *b) {
    memset(b, 0, sizeof(*b));
    if (off % 4 || off > len || len - off < 16 || word(image + off) != MARKER_START)
        return false;
    size_t limit = len - off < MAX_BLOCK ? len - off : MAX_BLOCK;
    size_t pos = 4;                     // from the block's start
    for (unsigned n = 0;; n++) {
        if (limit - pos < 12)           // an item header, then the link and the end marker
            return false;
        const uint8_t *item = image + off + pos;
        unsigned type = item[0];
        if (type == ITEM_LAST) {
            // Its size field is the words of all items before it.
            if (((unsigned)item[1] | (unsigned)item[2] << 8) * 4u != pos - 4)
                return false;
            b->link = (int32_t)word(item + 4);
            return word(item + 8) == MARKER_END;
        }
        size_t words = type & 0x80 ? ((size_t)item[1] | (size_t)item[2] << 8) : item[1];
        if (words == 0 || words * 4 > limit - pos)
            return false;
        if (n == 0 && type == ITEM_IMAGE_DEF)
            b->image_def = true;
        if (type == ITEM_SIGNATURE && words == SIG_WORDS && item[3] == SIG_SECP256K1)
            b->pubkey = item + 4;
        pos += words * 4;
    }
}

bootkey_image_t bootkey_image_pubkey(const uint8_t *image, size_t len, const uint8_t **pubkey) {
    *pubkey = NULL;
    // Every word-aligned start marker in the first 4 kB is tried as the first block until one
    // is the start of a closed loop. (Whether the boot ROM carries on past a start marker whose
    // loop doesn't close isn't documented; an image the tools made has only the real one.)
    for (size_t first = 0; first < FIRST_BLOCK_WITHIN && first + 16 <= len; first += 4) {
        block_t b;
        if (!block_at(image, len, first, &b))
            continue;
        // Follow the links. The loop must come back to the first block; the last image
        // definition met on the way is the one the boot ROM takes.
        size_t off = first;
        bool have_def = false;
        const uint8_t *key = NULL;
        for (unsigned n = 0; n < MAX_BLOCKS; n++) {
            if (b.image_def) {
                have_def = true;
                key = b.pubkey;
            }
            int64_t next = (int64_t)off + b.link;
            if (next == (int64_t)first) {
                if (!have_def)
                    break;              // a closed loop, but of something else
                *pubkey = key;
                return key ? BOOTKEY_IMAGE_SIGNED : BOOTKEY_IMAGE_UNSIGNED;
            }
            if (next < 0 || (uint64_t)next >= len || !block_at(image, len, (size_t)next, &b))
                break;                  // not a closed loop: not the first block after all
            off = (size_t)next;
        }
    }
    return BOOTKEY_IMAGE_NO_BLOCKS;
}

uint32_t bootkey_vote(const uint32_t *rows, unsigned n, unsigned need) {
    uint32_t out = 0;
    for (unsigned bit = 0; bit < 32; bit++) {
        unsigned set = 0;
        for (unsigned i = 0; i < n; i++)
            set += rows[i] >> bit & 1u;
        if (set >= need)
            out |= 1u << bit;
    }
    return out;
}

void bootkey_slot_hash(const uint16_t rows[BOOTKEY_SLOT_ROWS], uint8_t hash[BOOTKEY_HASH_LEN]) {
    for (unsigned k = 0; k < BOOTKEY_SLOT_ROWS; k++) {
        hash[2 * k] = (uint8_t)rows[k];
        hash[2 * k + 1] = (uint8_t)(rows[k] >> 8);
    }
}

bool bootkey_slot_usable(const bootkey_otp_t *otp, unsigned slot) {
    return slot < BOOTKEY_SLOTS && (otp->valid >> slot & 1u) && !(otp->invalid >> slot & 1u);
}

bool bootkey_slot_blank(const bootkey_otp_t *otp, unsigned slot) {
    for (unsigned i = 0; i < BOOTKEY_HASH_LEN; i++)
        if (otp->slot[slot][i])
            return false;
    return true;
}

bootkey_verdict_t bootkey_compare(const bootkey_otp_t *otp, const uint8_t hash[BOOTKEY_HASH_LEN],
                                  int *slot) {
    bootkey_verdict_t best = BOOTKEY_NO_MATCH;
    *slot = -1;
    for (unsigned s = 0; s < BOOTKEY_SLOTS; s++) {
        if (memcmp(otp->slot[s], hash, BOOTKEY_HASH_LEN))
            continue;
        bootkey_verdict_t v = bootkey_slot_usable(otp, s)        ? BOOTKEY_MATCH
                              : (otp->invalid >> s & 1u)          ? BOOTKEY_MATCH_INVALIDATED
                                                                  : BOOTKEY_MATCH_NOT_VALID;
        // A usable match wins; then "not valid yet" over "revoked"; then the lower slot.
        if (*slot < 0 || v < best) {
            best = v;
            *slot = (int)s;
        }
    }
    return best;
}
