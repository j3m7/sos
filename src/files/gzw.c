// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// gzw: one gzip member through zlib's deflate. See gzw.h.
//
// zlib (windowBits 12 + 16) writes the member: a header with no name and the date 0 (no
// deflateSetHeader() call), the deflate data, and the CRC-32 and length trailer. This file only
// hands it the input and the room, and turns "no progress possible" (Z_BUF_ERROR, a call with
// nothing to do) into success.

#include "files/gzw.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "files/gz.h"

static bool fail(gzw_t *w, const char *why) {
    w->error = why;
    return false;
}

bool gzw_init(gzw_t *w, int level) {
    memset(w, 0, sizeof(*w));
    w->z.zalloc = gz_zalloc;
    w->z.zfree = gz_zfree;
    if (level < 1 || level > 9)
        return fail(w, "bad level");
    if (deflateInit2(&w->z, level, Z_DEFLATED, GZW_WINDOW_BITS + 16, GZW_MEM_LEVEL,
                     Z_DEFAULT_STRATEGY) != Z_OK)
        return fail(w, "out of memory");
    w->inited = true;
    return true;
}

void gzw_end(gzw_t *w) {
    if (w->inited)
        deflateEnd(&w->z);
    w->inited = false;
}

bool gzw_feed(gzw_t *w, const uint8_t **in, size_t *in_len, bool last, uint8_t *out,
              size_t out_cap, size_t *out_len) {
    *out_len = 0;
    if (w->error)
        return false;
    if (!w->inited)
        return fail(w, "not started");
    if (w->done)
        return *in_len ? fail(w, "input after the end") : true;
    w->z.next_in = *in;
    w->z.avail_in = *in_len > UINT_MAX ? UINT_MAX : (uInt)*in_len;
    w->z.next_out = out;
    w->z.avail_out = out_cap > UINT_MAX ? UINT_MAX : (uInt)out_cap;
    uInt in_before = w->z.avail_in, out_before = w->z.avail_out;
    // With all the input given at once, Z_FINISH may need several calls (one per full `out`).
    bool finish = last && in_before == *in_len;
    int r = deflate(&w->z, finish ? Z_FINISH : Z_NO_FLUSH);
    size_t took = in_before - w->z.avail_in;
    *in += took;
    *in_len -= took;
    w->in_total += took;
    *out_len = out_before - w->z.avail_out;
    w->out_total += *out_len;
    if (r == Z_STREAM_END) {
        w->done = true;
        return true;
    }
    if (r == Z_OK || r == Z_BUF_ERROR)      // Z_BUF_ERROR: nothing to do, not an error
        return true;
    return fail(w, r == Z_MEM_ERROR ? "out of memory" : "deflate failed");
}

bool gzw_done(const gzw_t *w) {
    return w->done;
}

// Does `name` (length `len`) end in `suffix` (any case) after at least one character that isn't
// the end of a directory?
static bool ends_in(const char *name, size_t len, const char *suffix) {
    size_t n = strlen(suffix);
    return len > n && name[len - n - 1] != '/' && !strcasecmp(name + len - n, suffix);
}

bool gzw_has_gz_suffix(const char *name) {
    size_t len = strlen(name);
    return ends_in(name, len, ".gz") || ends_in(name, len, ".tgz");
}

bool gzw_output_name(const char *name, char *out, size_t n) {
    size_t len = strlen(name);
    if (!len || name[len - 1] == '/' || gzw_has_gz_suffix(name))
        return false;
    int w = snprintf(out, n, "%s.gz", name);
    return w >= 0 && (size_t)w < n;
}
