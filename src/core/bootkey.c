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
#define ITEM_LOAD_MAP  0x06
#define ITEM_HASH_DEF  0x47
#define ITEM_HASH_VALUE 0x4b
#define HASH_SHA256    0x01             // PICOBIN_HASH_SHA256
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

// The last image definition in the first closed loop of blocks, as the boot ROM takes it: true
// with its offset, false if there is no such loop.
static bool last_image_def(const uint8_t *image, size_t len, size_t *def) {
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
        for (unsigned n = 0; n < MAX_BLOCKS; n++) {
            if (b.image_def) {
                have_def = true;
                *def = off;
            }
            int64_t next = (int64_t)off + b.link;
            if (next == (int64_t)first) {
                if (!have_def)
                    break;              // a closed loop, but of something else
                return true;
            }
            if (next < 0 || (uint64_t)next >= len || !block_at(image, len, (size_t)next, &b))
                break;                  // not a closed loop: not the first block after all
            off = (size_t)next;
        }
    }
    return false;
}

bootkey_image_t bootkey_image_pubkey(const uint8_t *image, size_t len, const uint8_t **pubkey) {
    *pubkey = NULL;
    size_t def;
    block_t b;
    if (!last_image_def(image, len, &def) || !block_at(image, len, def, &b))
        return BOOTKEY_IMAGE_NO_BLOCKS;
    *pubkey = b.pubkey;
    return b.pubkey ? BOOTKEY_IMAGE_SIGNED : BOOTKEY_IMAGE_UNSIGNED;
}

// One load map entry (three words at `e`) as an offset into the image and a size. `item` is
// the LOAD_MAP item's offset, which a relative entry counts from.
static bool load_entry(size_t len, uint32_t base, size_t item,
                       bool absolute, const uint8_t *e, bootkey_load_t *out) {
    uint32_t stored = word(e), start = word(e + 4), third = word(e + 8);
    uint32_t size = absolute ? third - start : third;
    if (absolute && third < start)
        return false;
    if (size % 4)
        return false;
    out->size = size;
    if (stored == 0) {                  // the runtime range is filled with zeros
        out->storage = BOOTKEY_ZERO_FILL;
        return true;
    }
    uint32_t off = absolute ? stored - base : (uint32_t)item + stored;     // wraps as the ROM's
    if (off % 4 || off > len || size > len - off)
        return false;
    out->storage = off;
    return true;
}

bootkey_hash_kind_t bootkey_image_hash(const uint8_t *image, size_t len, uint32_t base,
                                       bootkey_hash_t *out) {
    memset(out, 0, sizeof(*out));
    size_t def;
    block_t b;
    if (!last_image_def(image, len, &def) || !block_at(image, len, def, &b))
        return BOOTKEY_HASH_NO_BLOCKS;
    out->block = def;
    out->signed_image = b.pubkey != NULL;
    // block_at() checked the items' sizes; walk them again for the hash items.
    size_t limit = len - def < MAX_BLOCK ? len - def : MAX_BLOCK;
    size_t pos = 4, map = 0, map_words = 0;
    bool have_map = false, have_def = false, have_value = false, unknown = false;
    for (;;) {
        const uint8_t *item = image + def + pos;
        unsigned type = item[0];
        if (type == ITEM_LAST)
            break;
        size_t words = type & 0x80 ? ((size_t)item[1] | (size_t)item[2] << 8) : item[1];
        if (type == ITEM_LOAD_MAP) {
            have_map = true;
            map = pos;
            map_words = words;
        } else if (type == ITEM_HASH_DEF) {
            if (words != 2 || item[3] != HASH_SHA256 || !have_map)
                unknown = true;
            out->block_words = (uint32_t)item[4] | (uint32_t)item[5] << 8;
            have_def = true;
            // The load map that counts is the most recent one before this item.
            if (have_map) {
                const uint8_t *m = image + def + map;
                unsigned entries = m[3] & 0x7fu;
                bool absolute = m[3] & 0x80u;
                if (map_words != 1 + 3u * entries || entries > BOOTKEY_LOAD_MAX) {
                    unknown = true;
                } else {
                    out->loads = entries;
                    for (unsigned i = 0; i < entries; i++)
                        if (!load_entry(len, base, def + map, absolute, m + 4 + 12 * i,
                                        &out->load[i]))
                            unknown = true;
                }
            }
        } else if (type == ITEM_HASH_VALUE) {
            if (words < 2 || words > 9 || !have_def)
                unknown = true;
            else {
                out->value_words = (unsigned)words - 1;
                for (unsigned i = 0; i < out->value_words; i++)
                    out->value[i] = word(item + 4 + 4 * i);
            }
            have_value = true;
        }
        pos += words * 4;
        if (pos >= limit)
            return BOOTKEY_HASH_UNKNOWN;    // block_at() wouldn't have let this happen
    }
    if (!have_value)
        return BOOTKEY_HASH_NONE;
    // The words hashed are the block's own, from its start marker: inside the block.
    if (unknown || out->block_words * 4u > pos + 12u)
        return BOOTKEY_HASH_UNKNOWN;
    return BOOTKEY_HASH_OK;
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
