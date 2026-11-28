// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Checksum lists and names for sha1sum, sha256sum and sha512sum: see sums_parse.h.

#include "files/sums_parse.h"

#include <string.h>

static bool is_control(char c) {
    return (unsigned char)c < 0x20 || c == 0x7f;
}

// --- names as printed --------------------------------------------------------------------------

unsigned sums_name_flags(const char *name) {
    unsigned flags = 0;
    for (const char *p = name; *p; p++) {
        if (*p == '\n' || *p == '\\')
            flags |= SUMS_NAME_ESCAPED;
        else if (is_control(*p))
            flags |= SUMS_NAME_CONTROL;
    }
    return flags;
}

const char *sums_name_char(char c) {
    if (c == '\n')
        return "\\n";
    if (c == '\\')
        return "\\\\";
    return is_control(c) ? "?" : NULL;
}

size_t sums_name_len(const char *name) {
    size_t n = 0;
    for (const char *p = name; *p; p++) {
        const char *s = sums_name_char(*p);
        n += s ? strlen(s) : 1;
    }
    return n;
}

// --- a list, line by line ----------------------------------------------------------------------

sums_next_t sums_lines_next(sums_lines_t *l, bool eof, char **line, size_t *len) {
    char *start = l->buf + l->used;
    char *nl = memchr(start, '\n', l->len - l->used);
    if (nl) {
        l->used = (size_t)(nl - l->buf) + 1;
        l->line++;
        if (l->skipping) {
            l->skipping = false;
            return SUMS_NEXT_LONG;
        }
        *nl = '\0';
        *line = start;
        *len = (size_t)(nl - start);
        return SUMS_NEXT_LINE;
    }
    // No '\n' in what's left: keep it at the start of the buffer.
    memmove(l->buf, start, l->len - l->used);
    l->len -= l->used;
    l->used = 0;
    if (l->skipping)
        l->len = 0;                     // still inside the long line
    if (l->len == sizeof(l->buf)) {     // full without a '\n': longer than SUMS_LINE_MAX
        l->skipping = true;
        l->len = 0;
    }
    if (eof) {
        if (l->skipping) {              // the long line was the last one
            l->skipping = false;
            l->line++;
            return SUMS_NEXT_LONG;
        }
        if (!l->len)
            return SUMS_NEXT_END;
        l->line++;                      // a last line without its '\n'
        *line = l->buf;
        *len = l->len;
        l->buf[l->len] = '\0';          // fits: len <= SUMS_LINE_MAX
        l->len = 0;
        return SUMS_NEXT_LINE;
    }
    return SUMS_NEXT_MORE;
}

char *sums_lines_room(sums_lines_t *l, size_t *room) {
    // After SUMS_NEXT_MORE, used is 0 and len < sizeof(buf).
    *room = sizeof(l->buf) - l->len;
    return l->buf + l->len;
}

// --- a line ------------------------------------------------------------------------------------

static int hex_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// Exactly `size` bytes of hex at p[0..2*size), into out.
static bool parse_hex(const char *p, size_t size, uint8_t *out) {
    for (size_t i = 0; i < size; i++) {
        int hi = hex_value(p[2 * i]), lo = hex_value(p[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

// Undo GNU's escape in place: "\\" and "\n" only. False for any other '\'.
static bool unescape(char *name) {
    char *w = name;
    for (const char *r = name; *r; r++) {
        if (*r != '\\') {
            *w++ = *r;
            continue;
        }
        r++;
        if (*r == '\\')
            *w++ = '\\';
        else if (*r == 'n')
            *w++ = '\n';
        else
            return false;               // includes a '\' at the end
    }
    *w = '\0';
    return true;
}

// "<tag> (<name>) = <hex>" in p[0..len), NUL-terminated at p[len]. The name ends at the last
// ") = ", so a name may hold one.
static bool parse_bsd(char *p, size_t len, const char *tag, size_t size, sums_line_t *out) {
    size_t tlen = strlen(tag), hlen = 2 * size;
    if (len < tlen + 2 + 1 + 4 + hlen || strncmp(p, tag, tlen) || strncmp(p + tlen, " (", 2))
        return false;
    char *hex = p + len - hlen;
    if (strncmp(hex - 4, ") = ", 4) || !parse_hex(hex, size, out->digest))
        return false;
    out->name = p + tlen + 2;
    hex[-4] = '\0';
    return true;
}

// "<hex>  <name>" or "<hex> *<name>".
static bool parse_gnu(char *p, size_t len, size_t size, sums_line_t *out) {
    size_t hlen = 2 * size;
    if (len < hlen + 3 || p[hlen] != ' ' || (p[hlen + 1] != ' ' && p[hlen + 1] != '*') ||
        !parse_hex(p, size, out->digest))
        return false;
    out->name = p + hlen + 2;
    return true;
}

sums_line_kind_t sums_parse_line(char *line, size_t len, const char *tag, size_t size,
                                 sums_line_t *out) {
    if (len && line[len - 1] == '\r')
        len--;
    line[len] = '\0';
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
        i++;
    if (i == len || line[0] == '#')
        return SUMS_LINE_SKIP;
    if (memchr(line, '\0', len) || size > SUMS_DIGEST_MAX)
        return SUMS_LINE_BAD;
    bool escaped = line[0] == '\\';
    char *p = line + escaped;
    size_t plen = len - escaped;
    if (!parse_bsd(p, plen, tag, size, out) && !parse_gnu(p, plen, size, out))
        return SUMS_LINE_BAD;
    if (escaped && !unescape(out->name))
        return SUMS_LINE_BAD;
    return out->name[0] ? SUMS_LINE_OK : SUMS_LINE_BAD;
}

// --- names in a list ---------------------------------------------------------------------------

// A component c[0..n) that FatFs may take as "..": dots and spaces only, two dots or more.
static bool is_dotdot(const char *c, size_t n) {
    size_t dots = 0;
    for (size_t i = 0; i < n; i++) {
        if (c[i] == '.')
            dots++;
        else if (c[i] != ' ')
            return false;
    }
    return dots >= 2;
}

bool sums_name_safe(const char *name) {
    if (name[0] == '/' || name[0] == '\\')
        return false;
    const char *start = name;
    for (const char *p = name;; p++) {
        if (*p == '\0' || *p == '/' || *p == '\\') {
            if (is_dotdot(start, (size_t)(p - start)))
                return false;
            if (*p == '\0')
                return true;
            start = p + 1;
        } else if (*p == ':' || is_control(*p)) {
            return false;
        }
    }
}
