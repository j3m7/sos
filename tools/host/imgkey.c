// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// imgkey <image.bin> : which key is this firmware image signed with? Used by tools/flash.sh.
//
// It finds the public key in the image's signature block with the firmware's own code
// (src/core/bootkey.c, the one the `bootkey` command uses on the board) and writes its 64 raw
// bytes to standard output; the caller hashes them (SHA-256 of those bytes is what a board's
// OTP holds). Exit status: 0 signed, 3 not signed, 4 not an image this code understands,
// 1 on a usage or file error.

#include <stdio.h>
#include <stdlib.h>

#include "core/bootkey.h"

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: imgkey <image.bin>\n");
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    size_t cap = 16u << 20, len;
    uint8_t *image = malloc(cap);
    if (!image) {
        fclose(f);
        return 1;
    }
    len = fread(image, 1, cap, f);
    int bad = ferror(f) || !feof(f);    // larger than any flash we have is not an image either
    fclose(f);
    if (bad) {
        fprintf(stderr, "imgkey: can't read %s (or it is larger than 16 MB)\n", argv[1]);
        free(image);
        return 1;
    }
    const uint8_t *pubkey;
    int status;
    switch (bootkey_image_pubkey(image, len, &pubkey)) {
    case BOOTKEY_IMAGE_SIGNED:
        status = fwrite(pubkey, 1, BOOTKEY_PUBKEY_LEN, stdout) == BOOTKEY_PUBKEY_LEN ? 0 : 1;
        break;
    case BOOTKEY_IMAGE_UNSIGNED:
        status = 3;
        break;
    default:
        status = 4;
    }
    free(image);
    return status;
}
