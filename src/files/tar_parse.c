// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "files/tar_parse.h"

#include <stdio.h>
#include <string.h>

#include "files/path.h"

// Header fields (offset, length): POSIX ustar, which GNU's layout shares up to the magic.
#define H_NAME      0
#define H_NAME_LEN  100
#define H_SIZE      124
#define H_MTIME     136
#define H_CHKSUM    148
#define H_NUM_LEN   12
#define H_CHK_LEN   8
#define H_TYPE      156
#define H_LINK      157
#define H_MAGIC     257
#define H_PREFIX    345
#define H_PREFIX_LEN 155

void tar_init(tar_t *t) {
    memset(t, 0, sizeof(*t));
}

static tar_event_t fail(tar_t *t, const char *why, uint64_t at) {
    t->state = TAR_S_FAILED;
    t->error = why;
    t->error_at = at;
    return TAR_ERROR;
}

// A numeric field: octal digits (leading spaces, then spaces or NULs to its end; all blank is
// 0), or GNU base-256 when the top bit of its first byte is set (two's complement, the next bit
// the sign; negative only if `sign_ok`). False if malformed or outside int64_t.
static bool number(const uint8_t *f, size_t n, bool sign_ok, int64_t *out) {
    if (f[0] & 0x80) {
        bool neg = (f[0] & 0x40) != 0;
        if (neg && !sign_ok)
            return false;
        uint8_t flip = neg ? 0xff : 0;
        uint64_t v = (uint64_t)((f[0] ^ flip) & 0x3f);
        for (size_t i = 1; i < n; i++) {
            if (v > (uint64_t)INT64_MAX >> 8)
                return false;
            v = v << 8 | (uint8_t)(f[i] ^ flip);
        }
        *out = neg ? -(int64_t)v - 1 : (int64_t)v;
        return true;
    }
    size_t i = 0;
    while (i < n && f[i] == ' ')
        i++;
    uint64_t v = 0;
    for (; i < n && f[i] >= '0' && f[i] <= '7'; i++) {
        if (v > (uint64_t)INT64_MAX >> 3)
            return false;
        v = v << 3 | (uint64_t)(f[i] - '0');
    }
    for (; i < n; i++)
        if (f[i] != ' ' && f[i] != '\0')
            return false;
    *out = (int64_t)v;
    return true;
}

// The checksum field against the block's sums with that field as spaces: the unsigned sum
// (POSIX), or the signed one (some old tars; GNU accepts both).
static bool checksum_ok(const uint8_t *b) {
    int64_t want;
    if ((b[H_CHKSUM] & 0x80) || !number(b + H_CHKSUM, H_CHK_LEN, false, &want))
        return false;
    int64_t u = 0, s = 0;
    for (size_t i = 0; i < TAR_BLOCK; i++) {
        uint8_t c = i >= H_CHKSUM && i < H_CHKSUM + H_CHK_LEN ? ' ' : b[i];
        u += c;
        s += (int8_t)c;
    }
    return want == u || want == s;
}

// Copy a field that ends at its first NUL or fills its length.
static size_t field(char *out, const uint8_t *f, size_t n) {
    size_t len = 0;
    while (len < n && f[len])
        len++;
    memcpy(out, f, len);
    out[len] = '\0';
    return len;
}

// A pax value for `path` or `linkpath` into `buf` (or `*set` false when empty: the header's
// field stands, as POSIX has it).
static void pax_name(tar_t *t, const char *v, size_t vlen, char *buf, bool *set) {
    *set = vlen > 0;
    if (!vlen)
        return;
    if (vlen >= TAR_NAME_MAX) {
        t->next_refused = "name too long";
        buf[0] = '\0';
    } else if (memchr(v, '\0', vlen)) {
        t->next_refused = "not a valid FAT name";
        buf[0] = '\0';
    } else {
        memcpy(buf, v, vlen);
        buf[vlen] = '\0';
    }
}

// A pax 'x' record set (meta_buf[0..meta_len)): "<len> <key>=<value>\n" each, len counting the
// whole record. False if malformed.
static bool pax_records(tar_t *t) {
    const char *b = t->meta_buf;
    size_t len = t->meta_len, p = 0;
    while (p < len) {
        size_t rlen = 0, q = p;
        for (; q < len && b[q] >= '0' && b[q] <= '9'; q++) {
            rlen = rlen * 10 + (size_t)(b[q] - '0');
            if (rlen > len)
                return false;
        }
        if (q == p || q >= len || b[q] != ' ' || rlen <= q - p + 1 || p + rlen > len ||
            b[p + rlen - 1] != '\n')
            return false;
        const char *key = b + q + 1, *end = b + p + rlen - 1;
        const char *eq = memchr(key, '=', (size_t)(end - key));
        if (!eq)
            return false;
        size_t klen = (size_t)(eq - key), vlen = (size_t)(end - eq - 1);
        const char *v = eq + 1;
        if (klen == 4 && !memcmp(key, "path", 4)) {
            pax_name(t, v, vlen, t->next_name_buf, &t->next_name);
        } else if (klen == 8 && !memcmp(key, "linkpath", 8)) {
            pax_name(t, v, vlen, t->next_link_buf, &t->next_link);
        } else if (klen == 4 && !memcmp(key, "size", 4)) {
            uint64_t n = 0;
            if (!vlen)
                return false;
            for (size_t i = 0; i < vlen; i++) {
                if (v[i] < '0' || v[i] > '9' || n > ((uint64_t)INT64_MAX - 9) / 10)
                    return false;
                n = n * 10 + (uint64_t)(v[i] - '0');
            }
            t->next_size = true;
            t->next_size_val = n;
        }
        p += rlen;
    }
    return true;
}

// A long name ('L') or link target ('K') record's data: the name up to its first NUL.
static void long_name(tar_t *t, char *buf, bool *set) {
    size_t len = strnlen(t->meta_buf, t->meta_len);
    *set = true;
    if (t->meta_over || len >= TAR_NAME_MAX) {
        t->next_refused = "name too long";
        buf[0] = '\0';
        return;
    }
    memcpy(buf, t->meta_buf, len);
    buf[len] = '\0';
}

// Data starts: `size` bytes, then the zeros to the next block.
static void start_data(tar_t *t, uint64_t size) {
    t->left = size;
    t->state = size ? TAR_S_DATA : TAR_S_HEADER;
}

// The data (or a record's) has all been taken: use the record, then skip the padding.
static tar_event_t end_data(tar_t *t, uint64_t size, uint64_t at) {
    char meta = t->meta;
    t->meta = 0;
    if (meta == 'L')
        long_name(t, t->next_name_buf, &t->next_name);
    else if (meta == 'K')
        long_name(t, t->next_link_buf, &t->next_link);
    else if (meta == 'x' && t->meta_over)
        t->next_refused = "pax header too large";
    else if (meta == 'x' && !pax_records(t))
        return fail(t, "bad pax header", at);
    t->left = (TAR_BLOCK - size % TAR_BLOCK) % TAR_BLOCK;
    t->state = t->left ? TAR_S_PAD : TAR_S_HEADER;
    return TAR_MORE;
}

static tar_kind_t kind_of(char type) {
    switch (type) {
    case '0': case '\0': case '7': return TAR_FILE;
    case '1': return TAR_HARDLINK;
    case '2': return TAR_SYMLINK;
    case '3': return TAR_CHARDEV;
    case '4': return TAR_BLOCKDEV;
    case '5': return TAR_DIR;
    case '6': return TAR_FIFO;
    default: return TAR_OTHER;
    }
}

// The size of the data being read: a record's or the entry's.
static uint64_t data_size(const tar_t *t) {
    return t->meta ? t->meta_size : t->entry.size;
}

// A whole header block is in t->block.
static tar_event_t header(tar_t *t) {
    const uint8_t *b = t->block;
    uint64_t at = t->pos - TAR_BLOCK;
    bool zero = true;
    for (size_t i = 0; zero && i < TAR_BLOCK; i++)
        zero = b[i] == 0;
    if (zero) {
        if (++t->zeros < 2)
            return TAR_MORE;
        t->state = TAR_S_END;
        return TAR_END;
    }
    t->zeros = 0;
    int64_t size, mtime;
    if (!checksum_ok(b) || !number(b + H_SIZE, H_NUM_LEN, false, &size))
        return fail(t, "bad header", at);
    if (!number(b + H_MTIME, H_NUM_LEN, true, &mtime))
        mtime = 0;
    char type = (char)b[H_TYPE];

    if (type == 'L' || type == 'K' || type == 'x' || type == 'g') {
        t->meta = type;
        t->meta_at = at;
        t->meta_size = (uint64_t)size;
        t->meta_len = 0;
        t->meta_over = false;
        start_data(t, (uint64_t)size);
        if (!size)
            return end_data(t, 0, at);
        return TAR_MORE;
    }

    tar_entry_t *e = &t->entry;
    e->type = type;
    e->kind = kind_of(type);
    e->offset = at;
    e->mtime = mtime;
    e->refused = t->next_refused;
    if (t->next_name) {
        strcpy(e->name, t->next_name_buf);
    } else {
        size_t len = 0;
        // POSIX ustar only: GNU keeps other fields where the prefix would be.
        if (!memcmp(b + H_MAGIC, "ustar\0", 6) && b[H_PREFIX]) {
            len = field(e->name, b + H_PREFIX, H_PREFIX_LEN);
            e->name[len++] = '/';
        }
        len += field(e->name + len, b + H_NAME, H_NAME_LEN);
        if (e->kind == TAR_FILE && type != '7' && len && e->name[len - 1] == '/')
            e->kind = TAR_DIR;              // v7's way of storing a directory
    }
    e->link[0] = '\0';
    if (e->kind == TAR_HARDLINK || e->kind == TAR_SYMLINK) {
        if (t->next_link)
            strcpy(e->link, t->next_link_buf);
        else
            field(e->link, b + H_LINK, H_NAME_LEN);
    }
    e->size = t->next_size ? t->next_size_val : (uint64_t)size;
    if (e->kind != TAR_FILE && e->kind != TAR_OTHER)
        e->size = 0;                        // no data, whatever the field says (as libarchive)
    t->next_name = t->next_link = t->next_size = false;
    t->next_refused = NULL;
    start_data(t, e->size);
    return TAR_HEADER;
}

tar_event_t tar_feed(tar_t *t, const uint8_t **in, size_t *in_len) {
    for (;;) {
        switch (t->state) {
        case TAR_S_FAILED:
            return TAR_ERROR;
        case TAR_S_END:
            t->pos += *in_len;
            *in += *in_len;
            *in_len = 0;
            return TAR_END;
        case TAR_S_PAD: {
            if (!*in_len)
                return TAR_MORE;
            size_t n = *in_len < t->left ? *in_len : (size_t)t->left;
            *in += n;
            *in_len -= n;
            t->pos += n;
            t->left -= n;
            if (!t->left)
                t->state = TAR_S_HEADER;
            break;
        }
        case TAR_S_DATA: {
            if (!*in_len)
                return TAR_MORE;
            size_t n = *in_len < t->left ? *in_len : (size_t)t->left;
            const uint8_t *p = *in;
            *in += n;
            *in_len -= n;
            t->pos += n;
            t->left -= n;
            if (t->meta) {
                if (t->meta_over || n > sizeof(t->meta_buf) - t->meta_len) {
                    t->meta_over = true;
                } else {
                    memcpy(t->meta_buf + t->meta_len, p, n);
                    t->meta_len += n;
                }
                if (!t->left && end_data(t, data_size(t), t->meta_at) == TAR_ERROR)
                    return TAR_ERROR;
                break;
            }
            t->data = p;
            t->data_len = n;
            if (!t->left)
                end_data(t, t->entry.size, t->entry.offset);
            return TAR_DATA;
        }
        case TAR_S_HEADER: {
            if (!*in_len)
                return TAR_MORE;
            size_t n = TAR_BLOCK - t->have;
            if (n > *in_len)
                n = *in_len;
            memcpy(t->block + t->have, *in, n);
            *in += n;
            *in_len -= n;
            t->pos += n;
            t->have += n;
            if (t->have < TAR_BLOCK)
                return TAR_MORE;
            t->have = 0;
            tar_event_t ev = header(t);
            if (ev == TAR_END)
                break;                      // TAR_S_END takes the rest
            if (ev != TAR_MORE)
                return ev;
            break;
        }
        }
    }
}

bool tar_entry_done(const tar_t *t) {
    return t->state != TAR_S_DATA || t->meta;
}

bool tar_finish(tar_t *t) {
    if (t->state == TAR_S_FAILED)
        return false;
    // One zero block and then the end: GNU takes it as the end, with a warning.
    if (t->state == TAR_S_END || (t->state == TAR_S_HEADER && !t->have && t->zeros == 1))
        return true;
    fail(t, "archive ends early", t->pos);
    return false;
}

// --- names ---------------------------------------------------------------------------------------

const char *tar_name_clean(const char *name, char *out, size_t n, bool *stripped) {
    if (n == 0)
        return "name too long";
    if (*name == '/') {
        *stripped = true;
        while (*name == '/')
            name++;
    }
    size_t len = 0;
    char comp[TAR_NAME_MAX];
    while (*name) {
        const char *start = name;
        while (*name && *name != '/')
            name++;
        size_t clen = (size_t)(name - start);
        while (*name == '/')
            name++;
        if (clen == 0 || (clen == 1 && start[0] == '.'))
            continue;
        if (clen == 2 && start[0] == '.' && start[1] == '.')
            return "unsafe path";
        if (clen >= sizeof(comp))
            return "name too long";
        memcpy(comp, start, clen);
        comp[clen] = '\0';
        const char *why = path_name_check(comp);
        if (why)
            return strcmp(why, "too long") ? "not a valid FAT name" : "name too long";
        if (len + (len ? 1 : 0) + clen + 1 > n)
            return "name too long";
        if (len)
            out[len++] = '/';
        memcpy(out + len, comp, clen);
        len += clen;
    }
    out[len] = '\0';
    return NULL;
}

const char *tar_kind_skipped(tar_kind_t kind, char type, char *buf, size_t n) {
    switch (kind) {
    case TAR_FILE:
    case TAR_DIR:
        return NULL;
    case TAR_HARDLINK:
    case TAR_SYMLINK:
        return "link skipped (FAT has no links)";
    case TAR_CHARDEV:
        return "character device skipped";
    case TAR_BLOCKDEV:
        return "block device skipped";
    case TAR_FIFO:
        return "FIFO skipped";
    case TAR_OTHER:
        break;
    }
    if (type > 0x20 && type < 0x7f)
        snprintf(buf, n, "type '%c' skipped", type);
    else
        snprintf(buf, n, "type 0x%02x skipped", (unsigned)(uint8_t)type);
    return buf;
}
