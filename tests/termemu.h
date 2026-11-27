// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// A tiny terminal for the full-screen programs' tests (the editor, the pager): cursor movement,
// erase, and printable bytes (UTF-8 as one cell), so a test checks what a terminal would show.
// Included by one test file at a time (everything here is static).

#ifndef SOS_TERMEMU_H
#define SOS_TERMEMU_H

#include <stdint.h>
#include <string.h>

#include "edit/screen.h"
#include "test.h"

// --- a tiny terminal: cursor movement, erase, and printable bytes (UTF-8 as one cell). Like a
// real one with autowrap off, the cursor stays on the last column after writing there. ------

#define T_ROWS SCREEN_ROWS_MAX
#define T_COLS SCREEN_COLS_MAX

static int t_row, t_col, t_rows, t_cols;
static bool t_cursor_on;
static size_t t_bytes;                      // output received in total
static size_t t_held;                       // bytes of a cut sequence waiting for the rest

// One character per cell, plus up to 3 combining marks; the right half of a wide character is
// WIDE_RIGHT.
#define CELL 17
#define WIDE_RIGHT "\x01"
static char screen[T_ROWS][T_COLS][CELL];

// The test terminal's own idea of width, independent of charwidth.c: a few well-known ranges.
static int t_width(uint32_t cp) {
    if ((cp >= 0x300 && cp <= 0x36F) || cp == 0x200D)
        return 0;
    if ((cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xFF01 && cp <= 0xFF60) || (cp >= 0x1F300 && cp <= 0x1F64F))
        return 2;
    return 1;
}

static uint32_t t_decode(const unsigned char *s, size_t l) {
    if (l == 1)
        return s[0];
    uint32_t v = s[0] & (0x7f >> l);
    for (size_t k = 1; k < l; k++)
        v = v << 6 | (s[k] & 0x3f);
    return v;
}

// Overwriting either half of a wide character blanks the other half, as terminals do.
static void t_clear_partner(int r, int c) {
    if (!strcmp(screen[r][c], WIDE_RIGHT) && c > 0)
        strcpy(screen[r][c - 1], " ");
    else if (c + 1 < t_cols && !strcmp(screen[r][c + 1], WIDE_RIGHT))
        strcpy(screen[r][c + 1], " ");
}

static void t_reset(int rows, int cols) {
    memset(screen, 0, sizeof(screen));
    t_row = t_col = 0;
    t_rows = rows;
    t_cols = cols;
    t_cursor_on = true;
    t_bytes = 0;
    t_held = 0;
}

// Act on the output in s: how many bytes were taken (a sequence or character cut short at the
// end is left for later).
static size_t t_parse(const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1b) {
            if (i + 1 >= n)
                return i;
            CHECK(s[i + 1] == '[');
            size_t j = i + 2;
            bool priv = j < n && s[j] == '?';
            if (priv)
                j++;
            int p[2] = { 0, 0 }, np = 0;
            while (j < n && ((s[j] >= '0' && s[j] <= '9') || s[j] == ';')) {
                if (s[j] == ';')
                    np++;
                else
                    p[np < 2 ? np : 1] = p[np < 2 ? np : 1] * 10 + (s[j] - '0');
                j++;
            }
            if (j >= n)
                return i;
            char f = s[j];
            if (priv && p[0] == 25)
                t_cursor_on = f == 'h';
            else if (f == 'H') {
                t_row = (p[0] ? p[0] : 1) - 1;
                t_col = (p[1] ? p[1] : 1) - 1;
                CHECK(t_row < t_rows && t_col < t_cols);
            } else if (f == 'K') {
                t_clear_partner(t_row, t_col);
                for (int k = t_col; k < t_cols; k++)
                    screen[t_row][k][0] = '\0';
            } else if (f == 'J') {
                memset(screen, 0, sizeof(screen));
            } else if (f == 'm') {
                // attributes: ignored
            } else {
                CHECK(!"unexpected escape sequence");
            }
            i = j + 1;
            continue;
        }
        CHECK(c >= 0x20 && c != 0x7f);      // nothing else a terminal could act on
        CHECK(!(c >= 0x80 && c <= 0x9f) || i > 0);
        size_t l = c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;
        if (i + l > n)
            return i;
        int w = t_width(t_decode((const unsigned char *)s + i, l));
        static bool last_col_full;          // the character before was written in the last column
        if (w == 0) {                       // joins the cell before (the cursor's, at the edge)
            int col = t_col == t_cols - 1 && last_col_full ? t_col : t_col - 1;
            if (col >= 0 && !strcmp(screen[t_row][col], WIDE_RIGHT))
                col--;
            CHECK(col >= 0 && screen[t_row][col][0]);  // there's a character to join
            if (col >= 0 && strlen(screen[t_row][col]) + l < CELL)
                strncat(screen[t_row][col], s + i, l);
            i += l;
            continue;
        }
        // A wide character must fit: the editor never sends one to the last column.
        CHECK(w == 1 || t_col + 1 < t_cols);
        // Autowrap off (as edit.c sets it): the cursor stays in the last column, and what's
        // written there overwrites it, as on a real terminal.
        t_clear_partner(t_row, t_col);
        memcpy(screen[t_row][t_col], s + i, l);
        screen[t_row][t_col][l] = '\0';
        if (w == 2 && t_col + 1 < t_cols) {
            t_clear_partner(t_row, t_col + 1);
            strcpy(screen[t_row][t_col + 1], WIDE_RIGHT);
        }
        last_col_full = t_col + w >= t_cols;
        t_col += w;
        if (t_col > t_cols - 1)
            t_col = t_cols - 1;
        i += l;
    }
    return i;
}

// Output arrives in pieces, as from edit.c: like a terminal, keep a sequence or character cut at
// the end of one piece until the next brings the rest.
static void t_feed(const char *s, size_t n) {
    static char held[70000];
    t_bytes += n;
    CHECK(t_held + n <= sizeof(held));
    memcpy(held + t_held, s, n);
    t_held += n;
    size_t done = t_parse(held, t_held);
    CHECK(t_held - done < 32);              // only a sequence's tail waits
    memmove(held, held + done, t_held - done);
    t_held -= done;
}

// Row r (0-based) as a string, trailing blanks dropped.
static const char *t_line(int r) {
    static char buf[T_COLS * CELL + 1];
    size_t n = 0;
    for (int k = 0; k < t_cols; k++) {
        if (!strcmp(screen[r][k], WIDE_RIGHT))
            continue;
        const char *cell = screen[r][k][0] ? screen[r][k] : " ";
        size_t l = strlen(cell);
        memcpy(buf + n, cell, l);
        n += l;
    }
    while (n && buf[n - 1] == ' ')
        n--;
    buf[n] = '\0';
    return buf;
}

#endif
