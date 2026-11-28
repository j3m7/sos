// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "files/tar_write.h"

#include <string.h>

// Header fields (offset, length): POSIX ustar.
#define H_NAME       0
#define H_NAME_LEN   100
#define H_MODE       100
#define H_UID        108
#define H_GID        116
#define H_ID_LEN     8
#define H_SIZE       124
#define H_MTIME      136
#define H_NUM_LEN    12
#define H_CHKSUM     148
#define H_CHK_LEN    8
#define H_TYPE       156
#define H_MAGIC      257
#define H_DEVMAJOR   329
#define H_DEVMINOR   337
#define H_PREFIX     345
#define H_PREFIX_LEN 155

#define PAX_DIR      "PaxHeaders/"  // a pax header's own name: PaxHeaders/<base name>
#define PAX_KEY      " path="

// `v` as n - 1 octal digits and a NUL.
static void octal(uint8_t *f, size_t n, uint64_t v) {
    f[n - 1] = '\0';
    for (size_t i = n - 1; i-- > 0; v >>= 3)
        f[i] = (uint8_t)('0' + (v & 7));
}

// A size or a date: octal when it fits n - 1 digits, else GNU's base-256 (the top bit of the
// first byte set, the value big-endian in the rest).
static void number(uint8_t *f, size_t n, uint64_t v) {
    if (v < 1ull << (3 * (n - 1))) {
        octal(f, n, v);
        return;
    }
    for (size_t i = n; i-- > 1; v >>= 8)
        f[i] = (uint8_t)(v & 0xff);
    f[0] = 0x80;
}

// The longest start of s[0..len) no longer than `max` bytes that doesn't cut a UTF-8 character.
static size_t utf8_cut(const char *s, size_t len, size_t max) {
    if (len <= max)
        return len;
    size_t n = max;
    while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80)
        n--;
    return n;
}

// One header block: name and prefix as given (each fitting its field), the rest as the
// arguments say, and its checksum.
static void block(uint8_t *b, const char *name, size_t name_len, const char *prefix,
                  size_t prefix_len, char type, uint64_t size, int64_t mtime, unsigned mode) {
    memset(b, 0, TAR_BLOCK);
    memcpy(b + H_NAME, name, name_len);
    memcpy(b + H_PREFIX, prefix, prefix_len);
    octal(b + H_MODE, H_ID_LEN, mode);
    octal(b + H_UID, H_ID_LEN, 0);
    octal(b + H_GID, H_ID_LEN, 0);
    number(b + H_SIZE, H_NUM_LEN, size);
    number(b + H_MTIME, H_NUM_LEN, mtime > 0 ? (uint64_t)mtime : 0);
    b[H_TYPE] = (uint8_t)type;
    memcpy(b + H_MAGIC, "ustar\0" "00", 8);
    octal(b + H_DEVMAJOR, H_ID_LEN, 0);
    octal(b + H_DEVMINOR, H_ID_LEN, 0);
    memset(b + H_CHKSUM, ' ', H_CHK_LEN);
    unsigned sum = 0;
    for (size_t i = 0; i < TAR_BLOCK; i++)
        sum += b[i];
    octal(b + H_CHKSUM, H_CHK_LEN - 1, sum);   // six digits, a NUL and a space, as GNU writes
}

static size_t digits(size_t v) {
    size_t d = 1;
    for (; v >= 10; v /= 10)
        d++;
    return d;
}

// Write `v` in decimal at p; returns the digits written.
static size_t decimal(char *p, size_t v) {
    size_t d = digits(v);
    for (size_t i = d; i-- > 0; v /= 10)
        p[i] = (char)('0' + v % 10);
    return d;
}

// The pax 'x' header and its record "<len> path=<full>\n" for a name ustar can't hold, at out.
// Returns the bytes written (the header and the record padded to a block).
static size_t pax(uint8_t *out, const char *full, size_t len, int64_t mtime) {
    size_t body = strlen(PAX_KEY) + len + 1;
    size_t d = digits(body + 1);
    while (digits(body + d) != d)
        d++;
    size_t rec = body + d;
    char *r = (char *)out + TAR_BLOCK;
    memset(r, 0, rec + tarw_pad(rec));
    size_t p = decimal(r, rec);
    memcpy(r + p, PAX_KEY, strlen(PAX_KEY));
    p += strlen(PAX_KEY);
    memcpy(r + p, full, len);
    r[p + len] = '\n';

    // Its own name: PaxHeaders/<the last component>, cut to fit (only tars that don't know pax
    // ever show it, as a file).
    size_t end = len;
    if (end && full[end - 1] == '/')
        end--;
    size_t base = end;
    while (base && full[base - 1] != '/')
        base--;
    char name[H_NAME_LEN];
    size_t n = strlen(PAX_DIR);
    memcpy(name, PAX_DIR, n);
    size_t take = utf8_cut(full + base, end - base, H_NAME_LEN - n);
    memcpy(name + n, full + base, take);
    block(out, name, n + take, "", 0, 'x', rec, mtime, 0644);
    return TAR_BLOCK + rec + tarw_pad(rec);
}

size_t tarw_header(uint8_t *out, const char *name, bool dir, uint64_t size, int64_t mtime) {
    size_t len = strlen(name);
    if (!len || len > TAR_NAME_MAX - 2 || name[len - 1] == '/')
        return 0;
    char full[TAR_NAME_MAX];
    memcpy(full, name, len);
    if (dir)
        full[len++] = '/';
    full[len] = '\0';
    char type = dir ? '5' : '0';
    unsigned mode = dir ? 0755 : 0644;
    if (dir)
        size = 0;
    if (len <= H_NAME_LEN) {
        block(out, full, len, "", 0, type, size, mtime, mode);
        return TAR_BLOCK;
    }
    // prefix '/' name: the prefix up to 155 bytes, the name 1 to 100.
    size_t lo = len - H_NAME_LEN - 1, hi = len - 2 < H_PREFIX_LEN ? len - 2 : H_PREFIX_LEN;
    for (size_t i = hi; i >= lo && i > 0; i--) {
        if (full[i] == '/') {
            block(out, full + i + 1, len - i - 1, full, i, type, size, mtime, mode);
            return TAR_BLOCK;
        }
    }
    size_t at = pax(out, full, len, mtime);
    block(out + at, full, utf8_cut(full, len, H_NAME_LEN), "", 0, type, size, mtime, mode);
    return at + TAR_BLOCK;
}

size_t tarw_pad(uint64_t size) {
    return (size_t)((TAR_BLOCK - size % TAR_BLOCK) % TAR_BLOCK);
}

uint64_t tarw_end(uint64_t written) {
    uint64_t total = written + 2 * TAR_BLOCK;
    return 2 * TAR_BLOCK + (TARW_RECORD - total % TARW_RECORD) % TARW_RECORD;
}
