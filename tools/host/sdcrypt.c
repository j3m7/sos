// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Read an encrypted SOS SD card on a PC (see src/files/sdcrypt.h for the format), with the data
// key that an encrypted `sd format` prints on the device's console when it finishes. Uses OpenSSL's libcrypto, so it's also an
// independent check of the device's encryption.
//
//   cc -O2 -o sdcrypt tools/host/sdcrypt.c -lcrypto
//   sdcrypt info <card-or-image>
//   sdcrypt decrypt <card-or-image> <key-file> <out-image>
//
// The key file holds the 64 hex digits `sd format` printed (`-` reads them from stdin), so the key
// doesn't end up in the shell history or the process list.
//
// `decrypt` writes the plain volume: a partition table and one FAT/exFAT partition, as `sd
// format` made it. `fdisk -l out-image` shows where the partition starts; Linux mounts it with
// `sudo mount -o loop,offset=$((<start>*512)) out-image /mnt`. On Linux, cryptsetup can also
// open the card directly, without a copy: see `sdcrypt info`.

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/evp.h>

#include "sdcrypt_tool.h"

#define SECTOR      512
#define KEY_LEN     32
#define HDR_OFFSET  20
#define HDR_CARD_ID 24

static const uint8_t magic[16] = "SOS-CRYPT-CARD\0";

static uint32_t get_le32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// XTS-AES-128 of `count` sectors from logical sector `lsector` (tweak: the sector number,
// little-endian, as dm-crypt's plain64). Returns 0 on success.
int sdcrypt_tool_xts(int encrypt, const uint8_t key[KEY_LEN], uint64_t lsector,
                     const uint8_t *in, uint8_t *out, unsigned count) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return -1;
    int rc = 0;
    for (unsigned i = 0; i < count && !rc; i++) {
        uint8_t iv[16] = {0};
        uint64_t s = lsector + i;
        for (int b = 0; b < 8; b++)
            iv[b] = (uint8_t)(s >> (8 * b));
        int n = 0;
        if (EVP_CipherInit_ex(ctx, EVP_aes_128_xts(), NULL, key, iv, encrypt) != 1 ||
            EVP_CipherUpdate(ctx, out + i * SECTOR, &n, in + i * SECTOR, SECTOR) != 1 || n != SECTOR)
            rc = -1;
    }
    EVP_CIPHER_CTX_free(ctx);
    return rc;
}

// Parse a header sector: 0 and the data offset, or -1 if it isn't one this tool knows.
int sdcrypt_tool_header(const uint8_t sector[SECTOR], uint32_t *offset) {
    if (memcmp(sector, magic, sizeof(magic)) || sector[16] != 1 || sector[17] != 1)
        return -1;
    *offset = get_le32(sector + HDR_OFFSET);
    return *offset ? 0 : -1;
}

#ifndef SDCRYPT_TOOL_NO_MAIN

// A hex digit's value, or -1. (sscanf's %2x would also take a space, a sign or "0x".)
static int hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int parse_key(const char *hex, uint8_t key[KEY_LEN]) {
    if (strlen(hex) != 2 * KEY_LEN)
        return -1;
    for (int i = 0; i < KEY_LEN; i++) {
        int hi = hex_digit(hex[2 * i]), lo = hex_digit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        key[i] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}

static int read_header(FILE *f, const char *path, uint32_t *offset) {
    uint8_t sector[SECTOR];
    // The header, or its backup copy at sector 4096.
    static const long where[] = { 0, 4096 };
    for (int i = 0; i < 2; i++) {
        if (fseek(f, where[i] * SECTOR, SEEK_SET) == 0 && fread(sector, 1, SECTOR, f) == SECTOR &&
            sdcrypt_tool_header(sector, offset) == 0) {
            if (i)
                fprintf(stderr, "%s: using the backup header (sector 4096)\n", path);
            printf("card id: ");
            for (int b = 0; b < 16; b++)
                printf("%02x", sector[HDR_CARD_ID + b]);
            printf("\n");
            return 0;
        }
    }
    fprintf(stderr, "%s: not an encrypted SOS card\n", path);
    return -1;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "info")) {
        FILE *f = fopen(argv[2], "rb");
        uint32_t offset;
        if (!f) {
            fprintf(stderr, "%s: %s\n", argv[2], strerror(errno));
            return 1;
        }
        int rc = read_header(f, argv[2], &offset);
        fclose(f);
        if (rc)
            return 1;
        printf("cipher:  XTS-AES-128, volume from sector %u\n", offset);
        printf("open on Linux (key: 32 bytes, from the hex `sd format` printed):\n"
               "  xxd -r -p key.txt > key.bin          # key.txt: the hex from `sd format`\n"
               "  sudo cryptsetup open --type plain --cipher aes-xts-plain64 --key-size 256 \\\n"
               "       --offset %u --key-file key.bin %s sos\n"
               "  sudo fdisk -l /dev/mapper/sos      # the partition's start sector\n"
               "  sudo mount -o offset=$((<start>*512)) /dev/mapper/sos /mnt\n",
               offset, argv[2]);
        return 0;
    }
    if (argc != 5 || strcmp(argv[1], "decrypt")) {
        fprintf(stderr, "usage: sdcrypt info <card-or-image>\n"
                        "       sdcrypt decrypt <card-or-image> <key-file|-> <out-image>\n");
        return 2;
    }
    uint8_t key[KEY_LEN];
    char hex[2 * KEY_LEN + 2] = "";
    FILE *kf = strcmp(argv[3], "-") ? fopen(argv[3], "r") : stdin;
    if (!kf) {
        fprintf(stderr, "%s: %s\n", argv[3], strerror(errno));
        return 1;
    }
    if (fscanf(kf, "%65s", hex) != 1)
        hex[0] = '\0';
    if (kf != stdin)
        fclose(kf);
    if (parse_key(hex, key)) {
        fprintf(stderr, "the key file must hold the 64 hex digits `sd format` printed\n");
        return 2;
    }
    FILE *in = fopen(argv[2], "rb");
    if (!in) {
        fprintf(stderr, "%s: %s\n", argv[2], strerror(errno));
        return 1;
    }
    uint32_t offset;
    if (read_header(in, argv[2], &offset)) {
        fclose(in);
        return 1;
    }
    FILE *out = fopen(argv[4], "wb");
    if (!out) {
        fprintf(stderr, "%s: %s\n", argv[4], strerror(errno));
        fclose(in);
        return 1;
    }
    if (fseeko(in, (off_t)offset * SECTOR, SEEK_SET)) {
        fprintf(stderr, "%s: can't seek to the volume\n", argv[2]);
        return 1;
    }
    static uint8_t buf[64 * SECTOR], plain[64 * SECTOR];
    uint64_t lsector = 0;
    size_t n;
    while ((n = fread(buf, SECTOR, 64, in)) > 0) {
        if (sdcrypt_tool_xts(0, key, lsector, buf, plain, (unsigned)n) ||
            fwrite(plain, SECTOR, n, out) != n) {
            fprintf(stderr, "decrypt/write failed at sector %llu\n", (unsigned long long)lsector);
            return 1;
        }
        lsector += n;
    }
    fclose(in);
    if (fclose(out)) {
        fprintf(stderr, "%s: %s\n", argv[4], strerror(errno));
        return 1;
    }
    fprintf(stderr, "wrote %llu sectors (%llu MB); `fdisk -l` shows where its partition starts\n",
            (unsigned long long)lsector, (unsigned long long)(lsector / 2048));
    return 0;
}

#endif
