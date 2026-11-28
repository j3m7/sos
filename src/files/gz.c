// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// gz: gzip members through zlib's inflate. See gz.h.
//
// zlib (windowBits 15 + 16) reads one member: its header, the deflate data, and the trailer,
// whose CRC-32 and length it checks. Between members this file looks at the bytes itself: a
// member starts with 1f 8b, which it collects (they can come one call apart) and then hands to
// zlib; zero bytes after a member are padding to the end; anything else is not gzip data.

#include "files/gz.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "core/heaptag.h"

void *gz_zalloc(void *opaque, unsigned items, unsigned size) {
    (void)opaque;
    if (size && items > SIZE_MAX / size)
        return NULL;
    return heap_alloc(HEAP_ZLIB, (size_t)items * size);
}

void gz_zfree(void *opaque, void *p) {
    (void)opaque;
    heap_release(HEAP_ZLIB, p);
}

static bool fail(gz_t *g, const char *why) {
    g->state = GZ_FAILED;
    g->error = why;
    return false;
}

// What zlib's result means, in gunzip's words. zlib's messages for corrupt data ("invalid block
// type", "invalid distance too far back") are kept as they are.
static const char *zlib_error(const gz_t *g, int r) {
    if (r == Z_MEM_ERROR)
        return "out of memory";
    if (r != Z_DATA_ERROR || !g->z.msg)
        return "corrupt data";
    if (!strcmp(g->z.msg, "incorrect data check"))
        return "CRC error: the data is corrupt";
    if (!strcmp(g->z.msg, "incorrect length check"))
        return "length error: the data is corrupt";
    return g->z.msg;
}

bool gz_init(gz_t *g) {
    memset(g, 0, sizeof(*g));
    g->z.zalloc = gz_zalloc;
    g->z.zfree = gz_zfree;
    if (inflateInit2(&g->z, 15 + 16) != Z_OK)          // gzip only, a window of up to 32 KB
        return fail(g, "out of memory");
    g->inited = true;
    g->state = GZ_MAGIC;
    return true;
}

void gz_end(gz_t *g) {
    if (g->inited)
        inflateEnd(&g->z);
    g->inited = false;
}

static void take(gz_t *g, const uint8_t **in, size_t *in_len, size_t n) {
    *in += n;
    *in_len -= n;
    g->in_total += n;
}

// Run zlib over `next_in`/`avail_in` (already set) into the rest of `out`; what it made is
// added to *out_len. Returns zlib's result.
static int run_inflate(gz_t *g, uint8_t *out, size_t out_cap, size_t *out_len) {
    size_t room = out_cap - *out_len;
    g->z.next_out = out + *out_len;
    g->z.avail_out = room > UINT_MAX ? UINT_MAX : (uInt)room;
    uInt before = g->z.avail_out;
    int r = inflate(&g->z, Z_NO_FLUSH);
    size_t made = before - g->z.avail_out;
    *out_len += made;
    g->out_total += made;
    return r;
}

bool gz_feed(gz_t *g, const uint8_t **in, size_t *in_len, uint8_t *out, size_t out_cap,
             size_t *out_len) {
    *out_len = 0;
    while (g->state != GZ_FAILED) {
        if (g->state == GZ_ZEROS) {
            size_t n = 0;
            while (n < *in_len && (*in)[n] == 0)
                n++;
            take(g, in, in_len, n);
            if (*in_len)
                return fail(g, "trailing garbage after the data");
            return true;
        }

        if (g->state == GZ_MAGIC) {
            if (!*in_len)
                return true;
            uint8_t b = **in;
            if (g->magic_len == 0 && b == 0 && g->members) {
                g->state = GZ_ZEROS;                // padding after the last member
                g->trailing_zeros = true;
                continue;
            }
            take(g, in, in_len, 1);
            g->magic[g->magic_len++] = b;
            if (b != (g->magic_len == 1 ? 0x1f : 0x8b))
                return fail(g, g->members ? "trailing garbage after the data"
                                          : "not in gzip format");
            if (g->magic_len < 2)
                continue;
            g->z.next_in = g->magic;            // a member: zlib reads its header from here on
            g->z.avail_in = 2;
            int r = run_inflate(g, out, out_cap, out_len);
            if (r != Z_OK || g->z.avail_in)
                return fail(g, zlib_error(g, r));
            g->state = GZ_MEMBER;
            continue;
        }

        // GZ_MEMBER
        if (*out_len == out_cap)
            return true;                        // full: gz_pending() says whether more is held
        g->z.next_in = *in;
        g->z.avail_in = *in_len > UINT_MAX ? UINT_MAX : (uInt)*in_len;
        uInt before = g->z.avail_in;
        int r = run_inflate(g, out, out_cap, out_len);
        take(g, in, in_len, before - g->z.avail_in);
        if (r == Z_STREAM_END) {                // the trailer matched: the next member, if any
            g->members++;
            inflateReset(&g->z);
            g->state = GZ_MAGIC;
            g->magic_len = 0;
            continue;
        }
        if (r != Z_OK && r != Z_BUF_ERROR)      // Z_BUF_ERROR: no progress possible, not an error
            return fail(g, zlib_error(g, r));
        if (!*in_len || g->z.avail_out == 0)
            return true;                        // needs input, or `out` is full
    }
    return false;
}

bool gz_pending(const gz_t *g) {
    // zlib stopped on a full buffer last time (avail_out 0) and may hold more: a match being
    // copied, a stored block, the end of the member not yet seen.
    return g->state == GZ_MEMBER && g->z.avail_out == 0;
}

bool gz_finish(gz_t *g) {
    switch (g->state) {
    case GZ_FAILED:
        return false;
    case GZ_ZEROS:
        return true;
    case GZ_MAGIC:
        if (g->members && g->magic_len == 0)
            return true;
        return fail(g, g->in_total ? "unexpected end of file" : "empty file");
    case GZ_MEMBER:
    default:
        return fail(g, "unexpected end of file");
    }
}

// Does `name` (length `len`) end in `suffix` (any case) after at least one character that isn't
// the end of a directory?
static bool ends_in(const char *name, size_t len, const char *suffix) {
    size_t n = strlen(suffix);
    return len > n && name[len - n - 1] != '/' && !strcasecmp(name + len - n, suffix);
}

bool gz_output_name(const char *name, char *out, size_t n) {
    size_t len = strlen(name);
    size_t keep;
    const char *add;
    if (ends_in(name, len, ".gz")) {
        keep = len - 3;
        add = "";
    } else if (ends_in(name, len, ".tgz")) {
        keep = len - 4;
        add = ".tar";
    } else {
        return false;
    }
    int w = snprintf(out, n, "%.*s%s", (int)keep, name, add);
    return w >= 0 && (size_t)w < n;
}
