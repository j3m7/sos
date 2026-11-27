// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The screen the full-screen programs share (see screen.h): layout of one character, and a frame
// driver. Depends only on termtext.c, charwidth.c and the C library, so it builds on the host for
// unit tests.
//
// Width is a guess at what the terminal does: one built on an older Unicode can disagree about
// recent emoji, and sequences joined with ZWJ count as their separate characters. The terminal's
// autowrap should be off (fsterm.c does that), so a wide character can't push a row onto the
// next one.

#include "edit/screen.h"

#include <stdio.h>
#include <string.h>

#include "shell/charwidth.h"
#include "shell/termtext.h"

#define BAD_CP 0xFFFFFFFFu

// --- layout --------------------------------------------------------------------------------------

// The UTF-8 character at s: its length (1 for a byte that doesn't start a valid one) and code
// point (BAD_CP if invalid). Same rules as termtext.c: no overlongs, surrogates or > U+10FFFF.
static size_t decode_bytes(const unsigned char *s, size_t avail, uint32_t *cp) {
    unsigned c = s[0];
    *cp = BAD_CP;
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    unsigned need;
    uint32_t v;
    if (c >= 0xc2 && c <= 0xdf)
        need = 1, v = c & 0x1f;
    else if (c >= 0xe0 && c <= 0xef)
        need = 2, v = c & 0x0f;
    else if (c >= 0xf0 && c <= 0xf4)
        need = 3, v = c & 0x07;
    else
        return 1;
    if (need >= avail)
        return 1;
    for (unsigned i = 1; i <= need; i++) {
        unsigned b = s[i], lo = 0x80, hi = 0xbf;
        if (i == 1) {
            if (c == 0xe0) lo = 0xa0;       // overlong
            if (c == 0xed) hi = 0x9f;       // surrogates
            if (c == 0xf0) lo = 0x90;       // overlong
            if (c == 0xf4) hi = 0x8f;       // above U+10FFFF
        }
        if (b < lo || b > hi)
            return 1;
        v = v << 6 | (b & 0x3f);
    }
    *cp = v;
    return need + 1;
}

// Shown as itself (else as '?').
static bool shows(uint32_t cp) {
    return (cp >= 0x20 && cp < 0x7f) || (cp >= 0xa0 && cp != BAD_CP && !termtext_hidden(cp));
}

// A combining mark or joiner: it belongs to the character before it.
static bool joins(uint32_t cp) {
    return shows(cp) && charwidth(cp) == 0;
}

unit_t screen_unit(const unsigned char *s, size_t avail, size_t col) {
    unit_t u = { 0 };
    uint32_t cp;
    u.len = decode_bytes(s, avail, &cp);
    if (cp == '\t') {
        u.tab = true;
        u.width = SCREEN_TAB - col % SCREEN_TAB;
        return u;
    }
    int w = shows(cp) ? charwidth(cp) : 0;
    u.glyph = w > 0;
    u.width = w > 0 ? (size_t)w : 1;
    for (int m = 0; u.glyph && m < SCREEN_MAX_MARKS && u.len < avail; m++) {
        size_t l = decode_bytes(s + u.len, avail - u.len, &cp);
        if (!joins(cp))
            break;
        u.len += l;
    }
    return u;
}

size_t screen_put(char *out, size_t n, const char *s, size_t len) {
    memcpy(out + n, s, len);
    return n + len;
}

size_t screen_unit_out(char *out, size_t n, const unsigned char *s, unit_t u, size_t col,
                       size_t left, size_t right) {
    if (col + u.width <= left || col >= right)
        return n;
    size_t from = col < left ? left : col, to = col + u.width > right ? right : col + u.width;
    if (u.tab || to - from < u.width) {         // a tab, or cut by an edge
        for (size_t i = from; i < to; i++)
            out[n++] = ' ';
    } else if (u.glyph) {
        n = screen_put(out, n, (const char *)s, u.len);
    } else {
        out[n++] = '?';
    }
    return n;
}

size_t screen_put_text(char *out, size_t n, const char *s, size_t cols, size_t *used) {
    size_t len = strlen(s), col = 0;
    for (size_t p = 0; p < len;) {
        unit_t u = screen_unit((const unsigned char *)s + p, len - p, col);
        if (u.tab) {
            u.width = 1;
            u.glyph = false;
        }
        if (col + u.width > cols)
            break;
        if (out) {
            if (u.glyph)
                n = screen_put(out, n, s + p, u.len);
            else
                out[n++] = '?';
        }
        col += u.width;
        p += u.len;
    }
    if (used)
        *used = col;
    return n;
}

size_t screen_text(char *out, size_t n, const unsigned char *s, size_t len, size_t col_off, size_t cols) {
    size_t col = 0, right = col_off + cols;
    for (size_t p = 0; p < len && s[p] != '\n' && col < right;) {
        unit_t u = screen_unit(s + p, len - p, col);
        n = screen_unit_out(out, n, s + p, u, col, col_off, right);
        col += u.width;
        p += u.len;
    }
    return n;
}

size_t screen_row_start(int r, char *out) {
    return (size_t)snprintf(out, 16, "\x1b[%d;1H\x1b[K", r + 1);
}

// --- frames --------------------------------------------------------------------------------------

static uint32_t hash(const char *s, size_t n) {
    uint32_t h = 2166136261u;               // FNV-1a
    for (size_t i = 0; i < n; i++)
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

void screen_init(screen_t *s, screen_begin_fn begin, screen_row_fn render, screen_cursor_fn cursor,
                 void *ctx) {
    memset(s, 0, sizeof(*s));
    s->rows = 24;
    s->cols = 80;
    s->begin = begin;
    s->render = render;
    s->cursor = cursor;
    s->ctx = ctx;
    screen_invalidate(s);
}

void screen_changed(screen_t *s) {
    s->dirty = true;
    s->drawing = false;                     // restart the frame
}

void screen_invalidate(screen_t *s) {
    s->clear = true;
    memset(s->row_valid, 0, sizeof(s->row_valid));
    screen_changed(s);
}

void screen_resize(screen_t *s, int rows, int cols) {
    s->rows = rows < SCREEN_ROWS_MIN ? SCREEN_ROWS_MIN : rows > SCREEN_ROWS_MAX ? SCREEN_ROWS_MAX : rows;
    s->cols = cols < SCREEN_COLS_MIN ? SCREEN_COLS_MIN : cols > SCREEN_COLS_MAX ? SCREEN_COLS_MAX : cols;
    screen_invalidate(s);
}

bool screen_drawn(const screen_t *s) {
    return !s->dirty;
}

size_t screen_draw(screen_t *s, char *out, size_t cap) {
    size_t n = 0;
    if (!s->dirty)
        return 0;
    if (!s->drawing) {
        // A row cut short when the frame restarted goes out whole first: stale, but complete, so
        // the terminal never sees a sequence or character cut by a new one. It's drawn again
        // below (its row isn't valid).
        if (s->row_sent < s->row_len) {
            size_t take = s->row_len - s->row_sent;
            if (take > cap)
                take = cap;
            n = screen_put(out, n, s->row + s->row_sent, take);
            s->row_sent += take;
            if (s->row_sent < s->row_len)
                return n;
        }
        if (cap - n < 16)
            return n;
        if (s->begin)
            s->begin(s->ctx);
        s->drawing = true;
        s->draw_row = 0;
        n = screen_put(out, n, "\x1b[?25l", 6);    // hide the cursor while drawing
        if (s->clear) {
            n = screen_put(out, n, "\x1b[2J", 4);
            s->clear = false;
        }
    }
    while (s->draw_row < s->rows) {
        int r = s->draw_row;
        if (s->row_sent == s->row_len) {    // not in the middle of a row: render this one
            size_t len = s->render(s->ctx, r, s->row);
            uint32_t h = hash(s->row, len);
            if (s->row_valid[r] && s->drawn[r] != h)
                s->row_valid[r] = false;    // what it shows is wrong until the whole row is out
            s->drawn[r] = h;
            s->row_len = s->row_valid[r] ? 0 : len;
            s->row_sent = 0;
        }
        if (s->row_sent < s->row_len) {
            size_t take = s->row_len - s->row_sent;
            if (take > cap - n)
                take = cap - n;             // the rest on the next call
            n = screen_put(out, n, s->row + s->row_sent, take);
            s->row_sent += take;
            if (s->row_sent < s->row_len)
                return n;
            s->row_valid[r] = true;
        }
        s->draw_row++;
    }
    // Finally the cursor.
    char tail[32];
    int row_no = s->rows, col_no = 1;
    bool shown = false;
    s->cursor(s->ctx, &row_no, &col_no, &shown);
    size_t tl = (size_t)snprintf(tail, sizeof(tail), "\x1b[%d;%dH%s", row_no, col_no,
                                 shown ? "\x1b[?25h" : "");
    if (n + tl > cap)
        return n;
    n = screen_put(out, n, tail, tl);
    s->drawing = false;
    s->dirty = false;
    return n;
}
