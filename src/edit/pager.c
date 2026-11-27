// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The pager's model and screen (see pager.h). Depends only on screen.c, keys.h and the C library,
// so it builds on the host for unit tests.
//
// The text is read through a window: at() gives the bytes from an offset on, loading the window
// around that offset when they aren't in it, placed so that reading on (forward) or back from it
// has the most to go on. Everything else, finding line starts and ends, searching, counting,
// goes through at(), so a text of any size reads the same.

#include "edit/pager.h"

#include <stdio.h>
#include <string.h>

#include "core/heaptag.h"
#include "shell/keys.h"

#define CTRL(c) ((c) & 0x1f)
#define NO_POS  ((size_t)-1)

// --- the window ----------------------------------------------------------------------------------

// The bytes from off on: a pointer into the window and how many are there (at least min(want,
// what's left of the text, the window) of them), loading the window if it holds fewer: ending
// just past off when `backward`, so a scan back from off has the most to go on, else starting at
// off. A scan asks for 1 byte, so it gets whatever the window holds from off on and the window
// is loaded only at its edge; a unit or a match asks for what it needs in one piece. NULL after
// a failed read. Past the end of the text, a valid pointer with 0 bytes.
static const unsigned char *at(pager_t *p, size_t off, size_t want, bool backward, size_t *avail) {
    const unsigned char *win = (const unsigned char *)p->win;
    if (off >= p->src.size) {
        *avail = 0;
        return win;
    }
    size_t left = p->src.size - off;
    if (want > left)
        want = left;
    if (want > p->win_cap)
        want = p->win_cap;
    bool inside = off >= p->win_off && off < p->win_off + p->win_len;
    size_t have = inside ? p->win_off + p->win_len - off : 0;
    if (have < want) {
        size_t start = off;
        if (backward)
            start = off + 1 >= p->win_cap ? off + 1 - p->win_cap : 0;
        size_t n = p->src.size - start;
        if (n > p->win_cap)
            n = p->win_cap;
        size_t got = p->src.read(p->src.ctx, start, p->win, n);
        p->win_off = start;
        p->win_len = got;
        if (got < n && start + got <= off) {    // what was asked for didn't come
            p->failed = true;                   // the status row says so from now on
            *avail = 0;
            return NULL;
        }
        have = p->win_off + p->win_len - off;
    }
    *avail = have;
    return win + (off - p->win_off);
}

// --- lines ---------------------------------------------------------------------------------------

// The '\n' at or after off: its offset, or the end of the text.
static size_t find_nl(pager_t *p, size_t off) {
    while (off < p->src.size) {
        size_t avail;
        const unsigned char *s = at(p, off, 1, false, &avail);
        if (!s || !avail)
            break;
        const unsigned char *q = memchr(s, '\n', avail);
        if (q)
            return off + (size_t)(q - s);
        off += avail;
    }
    return p->src.size;
}

// The start of the line holding pos (pos <= size): just past the last '\n' before pos, or 0.
static size_t line_start(pager_t *p, size_t pos) {
    while (pos > 0) {
        size_t avail;
        if (!at(p, pos - 1, 1, true, &avail))
            return 0;
        // The window holds [win_off, pos): look back through it.
        const unsigned char *base = (const unsigned char *)p->win;
        for (size_t i = pos - p->win_off; i > 0; i--)
            if (base[i - 1] == '\n')
                return p->win_off + i;
        pos = p->win_off;
    }
    return 0;
}

// The start of the line after the one at off, or the end of the text (a text that ends with a
// '\n' has no empty line after it).
static size_t next_line(pager_t *p, size_t off) {
    size_t nl = find_nl(p, off);
    return nl < p->src.size ? nl + 1 : p->src.size;
}

static int text_rows(const pager_t *p) {
    return p->scr.rows - 1;
}

// The '\n's in [from, to), and the last byte there (*last, left alone for an empty range). False
// if a read failed. One pass over that part of the text.
static bool count_nls(pager_t *p, size_t from, size_t to, long *n, unsigned char *last) {
    size_t off = from;
    while (off < to) {
        size_t avail;
        const unsigned char *s = at(p, off, 1, false, &avail);
        if (!s || !avail)
            return false;
        if (avail > to - off)
            avail = to - off;
        for (const unsigned char *q = s, *stop = s + avail; (q = memchr(q, '\n', (size_t)(stop - q))) != NULL; q++)
            (*n)++;
        *last = s[avail - 1];
        off += avail;
    }
    return true;
}

// Lines in the text: '\n's, plus one for a last line without one. One pass over the text.
static long count_lines(pager_t *p) {
    if (p->lines >= 0)
        return p->lines;
    long n = 0;
    unsigned char last = '\n';
    bool ok = count_nls(p, 0, p->src.size, &n, &last);
    if (p->src.size > 0 && last != '\n')
        n++;
    if (ok && !p->failed)                   // a count cut short by a read error isn't kept
        p->lines = n;
    return n;
}

// The `top` that puts the last line on the bottom row (0 if the text fits), and how many lines
// are then on screen: found by walking back from the end, so only the tail of the text is read.
// Depends on the screen size: forgotten on resize.
static size_t end_top(pager_t *p) {
    if (p->end_top != NO_POS)
        return p->end_top;
    size_t size = p->src.size, start = 0;
    int shown = 0;
    if (size > 0) {
        size_t avail, end = size;
        const unsigned char *s = at(p, size - 1, 1, true, &avail);
        if (s && s[0] == '\n')
            end = size - 1;
        start = line_start(p, end);
        shown = 1;
        for (; shown < text_rows(p) && start > 0; shown++)
            start = line_start(p, start - 1);
    }
    p->end_top = start;
    p->end_shown = shown;
    p->end_top_line = -1;
    return start;
}

// The line number of end_top(): costs the one pass over the text that counts the lines, the
// first time.
static long end_line(pager_t *p) {
    end_top(p);
    if (p->end_top_line < 0) {
        p->end_top_line = count_lines(p) - p->end_shown;
        if (p->end_top_line < 0)
            p->end_top_line = 0;
    }
    return p->end_top_line;
}

// --- setup ---------------------------------------------------------------------------------------

static void changed(pager_t *p) {
    screen_changed(&p->scr);
}

void pager_message(pager_t *p, const char *msg) {
    snprintf(p->msg, sizeof(p->msg), "%s", msg);
    changed(p);
}

void pager_set_log_waiting(pager_t *p, int n) {
    if (n != p->log_waiting) {
        p->log_waiting = n;
        changed(p);
    }
}

static void begin_frame(void *ctx);
static size_t render_row(void *ctx, int r, char *out);
static void cursor_at(void *ctx, int *row, int *col, bool *shown);
static void go_end(pager_t *p);

bool pager_init(pager_t *p, const char *name, pager_source_t src, size_t window) {
    memset(p, 0, sizeof(*p));
    size_t nl = strlen(name);               // a long path: its end, where the file name is
    if (nl < sizeof(p->name))
        snprintf(p->name, sizeof(p->name), "%s", name);
    else
        snprintf(p->name, sizeof(p->name), "...%s", name + nl - (sizeof(p->name) - 4));
    p->src = src;
    p->lines = -1;
    p->end_top = NO_POS;
    if (!window)
        window = PAGER_WINDOW;
    p->growing = src.growing;
    p->win_cap = src.size < window && !src.growing ? src.size : window;
    p->win = heap_alloc(HEAP_EDIT, p->win_cap ? p->win_cap : 1);
    if (!p->win)
        return false;
    snprintf(p->msg, sizeof(p->msg), "q quits, h lists the keys");
    screen_init(&p->scr, begin_frame, render_row, cursor_at, p);
    return true;
}

void pager_free(pager_t *p) {
    heap_release(HEAP_EDIT, p->win);
    p->win = NULL;
    p->win_cap = p->win_len = 0;
}

void pager_resize(pager_t *p, int rows, int cols) {
    screen_resize(&p->scr, rows, cols);
    p->end_top = NO_POS;
    if (p->top > end_top(p))                // a taller terminal: rows past the end would show '~'
        go_end(p);
}

// The text now has `size` bytes. The line count is extended over the new bytes when it was known
// (a last line without its '\n' was counted, and is counted again with what follows it).
void pager_grow(pager_t *p, size_t size) {
    size_t old = p->src.size;
    if (size <= old)
        return;
    unsigned char before = '\n';
    if (p->lines >= 0 && old > 0) {
        size_t avail;
        const unsigned char *s = at(p, old - 1, 1, false, &avail);
        if (s && avail)
            before = s[0];
        else
            p->lines = -1;
    }
    p->src.size = size;
    p->end_top = NO_POS;
    p->end_top_line = -1;
    if (p->lines >= 0) {
        long n = before != '\n' ? p->lines - 1 : p->lines;
        unsigned char last = '\n';
        if (count_nls(p, old, size, &n, &last) && !p->failed) {
            p->lines = last != '\n' ? n + 1 : n;
        } else {
            p->lines = -1;
        }
    }
    if (p->follow)
        go_end(p);
    changed(p);                             // the end, or the percentage in the status row
}

void pager_set_growing(pager_t *p, bool growing) {
    p->growing = growing;
    if (!growing)
        p->follow = false;
    changed(p);
}

size_t pager_read_mem(void *ctx, size_t off, char *buf, size_t n) {
    memcpy(buf, (const char *)ctx + off, n);
    return n;
}

// --- moving --------------------------------------------------------------------------------------

static void move_down(pager_t *p, int k) {
    size_t stop = end_top(p);
    for (; k > 0 && p->top < stop; k--) {
        p->top = next_line(p, p->top);
        p->top_line++;
    }
}

static void move_up(pager_t *p, int k) {
    for (; k > 0 && p->top > 0; k--) {
        p->top = line_start(p, p->top - 1);
        p->top_line--;
    }
}

static void go_end(pager_t *p) {
    p->top = end_top(p);
    p->top_line = end_line(p);
}

// Does the text at off start with q (qn bytes)?
static bool matches(pager_t *p, size_t off, const char *q, size_t qn) {
    if (off + qn > p->src.size)
        return false;
    size_t avail;
    const unsigned char *s = at(p, off, qn, false, &avail);
    return s && avail >= qn && !memcmp(s, q, qn);
}

// Search [from, stop) for last_find: *hit is where it starts and *nls the '\n's before it from
// `from` (how many lines down it is).
static bool search(pager_t *p, size_t from, size_t stop, size_t *hit, long *nls) {
    const char *q = p->last_find;
    size_t qn = strlen(q), off = from;
    long count = 0;
    while (off < stop) {
        size_t avail;
        const unsigned char *s = at(p, off, 1, false, &avail);
        if (!s || !avail)
            return false;
        if (off + avail > stop)
            avail = stop - off;
        size_t i = 0;
        while (i < avail) {
            if (s[i] == (unsigned char)q[0]) {
                if (i + qn <= avail) {
                    if (!memcmp(s + i, q, qn)) {
                        *hit = off + i;
                        *nls = count;
                        return true;
                    }
                } else {                    // across the window's edge: at() may move it
                    bool m = matches(p, off + i, q, qn);
                    if (m) {
                        *hit = off + i;
                        *nls = count;
                        return true;
                    }
                    if (q[0] == '\n')
                        count++;
                    i++;
                    break;                  // s is stale: fetch again from here
                }
            }
            if (s[i] == '\n')
                count++;
            i++;
        }
        off += i;
    }
    return false;
}

// Show the line holding `hit`, with its number given by the '\n's before it.
static void show_line(pager_t *p, size_t hit, long line) {
    size_t start = line_start(p, hit), stop = end_top(p);
    if (start > stop) {                     // in the last screenful: show that
        p->top = stop;
        p->top_line = end_line(p);
    } else {
        p->top = start;
        p->top_line = line;
    }
}

// From the line after the top one, wrapping around to the start.
static void find_next(pager_t *p) {
    const char *q = p->find_len ? p->find : p->last_find;
    size_t qn = strlen(q);
    if (!qn) {
        pager_message(p, "");
        return;
    }
    if (q != p->last_find)
        memcpy(p->last_find, q, qn + 1);
    size_t from = next_line(p, p->top), hit;
    long nls;
    if (search(p, from, p->src.size, &hit, &nls)) {
        show_line(p, hit, p->top_line + (from > p->top ? 1 : 0) + nls);
        pager_message(p, "");
    } else if (search(p, 0, from, &hit, &nls)) {
        show_line(p, hit, nls);
        pager_message(p, "search wrapped around");
    } else {
        pager_message(p, "not found");
    }
}

// --- keys ----------------------------------------------------------------------------------------

static pager_action_t find_key(pager_t *p, int key) {
    if (key == '\r') {
        p->finding = false;
        find_next(p);
    } else if (key == CTRL('C') || key == CTRL('Q') || key == CTRL('G') || key == CTRL('F')) {
        p->finding = false;
        pager_message(p, "");
    } else if (key == 0x7f || key == '\b') {
        while (p->find_len && ((unsigned char)p->find[p->find_len - 1] & 0xc0) == 0x80)
            p->find_len--;                  // a whole UTF-8 character
        if (p->find_len)
            p->find_len--;
        p->find[p->find_len] = '\0';
    } else if (key >= 0x20 && key <= 0xff && key != 0x7f && p->find_len < PAGER_FIND_MAX) {
        p->find[p->find_len++] = (char)key;
        p->find[p->find_len] = '\0';
    }
    return PAGER_NONE;
}

pager_action_t pager_key(pager_t *p, int key) {
    changed(p);
    if (p->finding)
        return find_key(p, key);
    p->msg[0] = '\0';
    if (key != 'F' && key != 'h' && key != '?' && key != CTRL('L'))
        p->follow = false;
    int rows = text_rows(p);
    size_t step = (size_t)p->scr.cols / 2;

    switch (key) {
    case 'q':
    case 'Q':
    case CTRL('Q'):
    case CTRL('C'):
    case CTRL('X'):
        return PAGER_QUIT;
    case ' ':
    case 'f':
    case KEY_PGDN:
    case CTRL('V'):
        move_down(p, rows);
        break;
    case 'b':
    case KEY_PGUP:
    case CTRL('B'):
        move_up(p, rows);
        break;
    case 'd':
    case CTRL('D'):
        move_down(p, rows / 2);
        break;
    case 'u':
    case CTRL('U'):
        move_up(p, rows / 2);
        break;
    case 'j':
    case '\r':
    case KEY_DOWN:
    case CTRL('N'):
        move_down(p, 1);
        break;
    case 'k':
    case KEY_UP:
    case CTRL('P'):
        move_up(p, 1);
        break;
    case 'g':
    case '<':
    case KEY_HOME:
    case KEY_CTRL_HOME:
        p->top = 0;
        p->top_line = 0;
        break;
    case 'G':
    case '>':
    case KEY_END:
    case KEY_CTRL_END:
        go_end(p);
        break;
    case 'F':                               // as in less: stay on the end as text arrives
        if (p->growing)
            p->follow = true;
        go_end(p);
        break;
    case KEY_LEFT:
        p->col_off = p->col_off > step ? p->col_off - step : 0;
        break;
    case KEY_RIGHT:
        p->col_off += step;
        break;
    case '/':
    case CTRL('F'):
    case CTRL('W'):
        p->finding = true;
        p->find_len = 0;
        p->find[0] = '\0';
        break;
    case 'n':
        p->find_len = 0;
        find_next(p);
        break;
    case CTRL('L'):
        screen_invalidate(&p->scr);
        return PAGER_RESIZE;
    case 'h':
    case '?':
        // Growing: "F follow" takes the room of "d/u half"; d and u still work.
        pager_message(p, p->growing
                             ? "Space/b page, j/k line, g/G ends, F follow, / find, n next, q quit"
                             : "Space/b page, j/k line, d/u half, g/G ends, / find, n next, q quit");
        break;
    default:
        break;
    }
    return PAGER_NONE;
}

// --- screen --------------------------------------------------------------------------------------

static void begin_frame(void *ctx) {
    (void)ctx;
}

// One text row: the line starting at pos, from column col_off, through the window.
static size_t text_row(pager_t *p, size_t pos, char *out, size_t n) {
    size_t col = 0, left = p->col_off, right = left + (size_t)p->scr.cols;
    while (pos < p->src.size && col < right) {
        size_t avail;
        const unsigned char *s = at(p, pos, SCREEN_UNIT_MAX, false, &avail);
        if (!s || !avail || s[0] == '\n')
            break;
        unit_t u = screen_unit(s, avail, col);
        n = screen_unit_out(out, n, s, u, col, left, right);
        col += u.width;
        pos += u.len;
    }
    return n;
}

static size_t status_row(pager_t *p, char *out, size_t n) {
    size_t cols = (size_t)p->scr.cols;
    if (p->finding) {
        n = screen_put(out, n, "Search: ", 8);
        return screen_put_text(out, n, p->find, cols - 8, NULL);
    }
    if (p->msg[0])
        return screen_put_text(out, n, p->msg, cols, NULL);
    if (p->failed)
        return screen_put_text(out, n, "read error: the text can't be read past here (q quits)", cols, NULL);
    char left[96], right[96];
    snprintf(left, sizeof(left), " %s", p->name);
    int r = 0;
    if (p->log_waiting)
        r = snprintf(right, sizeof(right), "%d log message%s held (dmesg) | ", p->log_waiting,
                     p->log_waiting == 1 ? "" : "s");
    if (p->shown == 0)
        snprintf(right + r, sizeof(right) - (size_t)r, "empty ");
    else {
        r += snprintf(right + r, sizeof(right) - (size_t)r, "lines %ld-%ld", p->top_line + 1,
                      p->top_line + p->shown);
        if (p->growing)
            r += snprintf(right + r, sizeof(right) - (size_t)r, "/...");
        else if (p->lines >= 0)
            r += snprintf(right + r, sizeof(right) - (size_t)r, "/%ld", p->lines);
        if (p->follow)
            r += snprintf(right + r, sizeof(right) - (size_t)r, " FOLLOW");
        if (p->draw_past_end)
            snprintf(right + r, sizeof(right) - (size_t)r, p->growing ? " END so far " : " END ");
        else
            snprintf(right + r, sizeof(right) - (size_t)r, " %u%% ",
                     (unsigned)((unsigned long long)p->draw_pos * 100 / p->src.size));
    }
    size_t rl = strlen(right);
    if (rl > cols)
        rl = cols;
    n = screen_put(out, n, "\x1b[7m", 4);
    size_t used;
    n = screen_put_text(out, n, left, cols - rl, &used);
    for (; used < cols - rl; used++)
        out[n++] = ' ';
    n = screen_put(out, n, right + strlen(right) - rl, rl);
    return screen_put(out, n, "\x1b[m", 3);
}

static size_t render_row(void *ctx, int r, char *out) {
    pager_t *p = ctx;
    size_t n = screen_row_start(r, out);
    if (r == 0) {
        p->draw_pos = p->top;
        p->draw_past_end = p->top >= p->src.size;
        p->shown = 0;
    }
    if (r < text_rows(p)) {
        if (p->draw_past_end) {
            out[n++] = '~';
        } else {
            n = text_row(p, p->draw_pos, out, n);
            p->shown++;
            p->draw_pos = next_line(p, p->draw_pos);
            if (p->draw_pos >= p->src.size) {
                p->draw_past_end = true;
                if (p->lines < 0 && !p->failed)     // the end is on screen: the count is known
                    p->lines = p->top_line + p->shown;
            }
        }
    } else {
        n = status_row(p, out, n);
    }
    return n;
}

static void cursor_at(void *ctx, int *row, int *col, bool *shown) {
    pager_t *p = ctx;
    *row = p->scr.rows;
    *col = 1;
    *shown = p->finding;
    if (p->finding) {
        size_t used;
        screen_put_text(NULL, 0, p->find, (size_t)p->scr.cols - 8, &used);
        *col = 9 + (int)used;
        if (*col > p->scr.cols)
            *col = p->scr.cols;
    }
}
