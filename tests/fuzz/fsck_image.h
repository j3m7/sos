// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// A sparse disk image small enough to be a fuzz input, for fuzz_fsck.c and the seed check in
// test_fsck.c. A FAT32 volume needs 65526 clusters, so a whole image is far bigger than a fuzz
// input; but nearly all of it is zeros. The input lists only the non-zero bytes:
//
//   [sectors: u32]                         the device's size (capped at FSCK_IMAGE_DEVICE_MAX)
//   then records, to the end:
//   [lba: u32][offset: u16][len: u16][len bytes]   bytes from offset `offset` of sector `lba`
//
// Little-endian. A record may run on into the following sectors. Every sector no record
// touches reads as zeros; records past FSCK_IMAGE_SECTORS distinct sectors are dropped.
// tests/fuzz/seeds.py writes images this way.

#ifndef SOS_FSCK_IMAGE_H
#define SOS_FSCK_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define FSCK_IMAGE_SECTORS    128
#define FSCK_IMAGE_DEVICE_MAX (1u << 17)    // 64 MB: enough for FAT32 with 1-sector clusters

typedef struct {
    uint64_t sectors;
    unsigned n;
    uint64_t lba[FSCK_IMAGE_SECTORS];
    uint8_t data[FSCK_IMAGE_SECTORS][512];
} fsck_image_t;

static inline uint32_t fsck_image_take(const uint8_t **p, size_t *len, unsigned bytes) {
    uint32_t v = 0;
    for (unsigned i = 0; i < bytes; i++) {
        uint8_t b = 0;
        if (*len) {
            b = **p;
            (*p)++;
            (*len)--;
        }
        v |= (uint32_t)b << (8 * i);
    }
    return v;
}

static inline uint8_t *fsck_image_sector(fsck_image_t *im, uint64_t lba, bool create) {
    for (unsigned i = 0; i < im->n; i++)
        if (im->lba[i] == lba)
            return im->data[i];
    if (!create || im->n == FSCK_IMAGE_SECTORS)
        return NULL;
    im->lba[im->n] = lba;
    memset(im->data[im->n], 0, 512);
    return im->data[im->n++];
}

static inline void fsck_image_parse(fsck_image_t *im, const uint8_t *p, size_t len) {
    im->n = 0;
    im->sectors = fsck_image_take(&p, &len, 4);
    if (im->sectors > FSCK_IMAGE_DEVICE_MAX)
        im->sectors = FSCK_IMAGE_DEVICE_MAX;
    while (len) {
        uint64_t lba = fsck_image_take(&p, &len, 4);
        uint32_t ofs = fsck_image_take(&p, &len, 2) % 512;
        uint32_t n = fsck_image_take(&p, &len, 2);
        if (n > len)
            n = (uint32_t)len;
        while (n) {
            uint8_t *s = fsck_image_sector(im, lba, true);
            uint32_t k = 512 - ofs < n ? 512 - ofs : n;
            if (s)
                memcpy(s + ofs, p, k);
            p += k;
            len -= k;
            n -= k;
            lba++;
            ofs = 0;
        }
    }
}

// fsck_io_t's read callback over an fsck_image_t (ctx).
static inline bool fsck_image_read(void *ctx, uint64_t lba, uint32_t count, uint8_t *buf) {
    fsck_image_t *im = ctx;
    for (uint32_t i = 0; i < count; i++, lba++) {
        if (lba >= im->sectors)
            return false;
        const uint8_t *s = fsck_image_sector(im, lba, false);
        if (s)
            memcpy(buf + 512 * i, s, 512);
        else
            memset(buf + 512 * i, 0, 512);
    }
    return true;
}

#endif
