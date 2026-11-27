// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The full-screen editor's model and screen (see editor.h). Depends only on screen.c, keys.h
// and the C library, so it builds on the host for unit tests.
//
// Text is bytes in a gap buffer. A character takes the columns screen_unit() says (screen.h):
// two for wide CJK characters and most emoji, none for combining marks (an accent added to the
// letter before it), one otherwise; a tab goes up to the next multiple of EDITOR_TAB. The cursor
// moves by character, and a character with the combining marks after it (up to EDITOR_MAX_MARKS)
// moves and deletes as one; a mark beyond that, or with no character to go on, shows as '?', as
// does anything a terminal could act on or that would hide what the bytes are.
// Lines don't wrap: the view scrolls sideways to follow the cursor.

#include "edit/editor.h"

#include <stdio.h>
#include <string.h>

#include "core/heaptag.h"
#include "shell/keys.h"

#define CTRL(c) ((c) & 0x1f)

// --- gap buffer ----------------------------------------------------------------------------------

size_t editor_len(const editor_t *ed) {
    return ed->cap - (ed->gap_end - ed->gap);
}

char editor_at(const editor_t *ed, size_t i) {
    return i < ed->gap ? ed->buf[i] : ed->buf[i + (ed->gap_end - ed->gap)];
}

static unsigned char at(const editor_t *ed, size_t i) {
    return (unsigned char)editor_at(ed, i);
}

static void move_gap(editor_t *ed, size_t pos) {
    if (pos < ed->gap) {
        size_t n = ed->gap - pos;
        memmove(ed->buf + ed->gap_end - n, ed->buf + pos, n);
        ed->gap = pos;
        ed->gap_end -= n;
    } else if (pos > ed->gap) {
        size_t n = pos - ed->gap;
        memmove(ed->buf + ed->gap, ed->buf + ed->gap_end, n);
        ed->gap += n;
        ed->gap_end += n;
    }
}

// Room for n more bytes. The buffer is allocated at its limit when the editor opens, so it never
// grows: a realloc would hold the old and the new block at once, twice the text, when the limit
// (what the heap could spare, see edit.c) was set for one copy.
static bool reserve(const editor_t *ed, size_t n) {
    return ed->gap_end - ed->gap >= n;
}

static bool insert(editor_t *ed, const char *s, size_t n) {
    if (!reserve(ed, n)) {
        char m[80];                         // whole KB once rounding down loses under a tenth
        if (ed->limit >= 10 * 1024)
            snprintf(m, sizeof(m), "the text can't grow past %zu KB", ed->limit / 1024);
        else
            snprintf(m, sizeof(m), "the text can't grow past %zu bytes", ed->limit);
        editor_message(ed, m);
        return false;
    }
    move_gap(ed, ed->cursor);
    memcpy(ed->buf + ed->gap, s, n);
    ed->gap += n;
    ed->cursor += n;
    ed->modified = true;
    return true;
}

static void delete(editor_t *ed, size_t pos, size_t n) {
    move_gap(ed, pos);
    ed->gap_end += n;
    ed->modified = true;
}

// --- characters and lines ------------------------------------------------------------------------

// The unit starting at pos (not a '\n'), at column col.
static size_t unit_bytes_at(const editor_t *ed, size_t pos, unsigned char s[SCREEN_UNIT_MAX]) {
    size_t avail = editor_len(ed) - pos;
    if (avail > SCREEN_UNIT_MAX)
        avail = SCREEN_UNIT_MAX;
    for (size_t i = 0; i < avail; i++)
        s[i] = at(ed, pos + i);
    return avail;
}

static unit_t unit_at(const editor_t *ed, size_t pos, size_t col) {
    unsigned char s[SCREEN_UNIT_MAX];
    return screen_unit(s, unit_bytes_at(ed, pos, s), col);
}

// Past the unit at pos (a character and its marks), or past the '\n' there.
static size_t next_pos(const editor_t *ed, size_t pos) {
    if (pos >= editor_len(ed))
        return pos;
    if (at(ed, pos) == '\n')
        return pos + 1;
    return pos + unit_at(ed, pos, 0).len;   // a unit's length doesn't depend on its column
}

static size_t line_start(const editor_t *ed, size_t pos);

// The start of the unit before pos: the '\n' before a line, or found by walking the line from
// its start (a mark's unit depends on what's before it).
static size_t prev_pos(const editor_t *ed, size_t pos) {
    if (pos == 0)
        return 0;
    if (at(ed, pos - 1) == '\n')
        return pos - 1;
    size_t p = line_start(ed, pos), last = p;
    while (p < pos) {
        last = p;
        p = next_pos(ed, p);
    }
    return last;
}

static size_t line_start(const editor_t *ed, size_t pos) {
    while (pos > 0 && at(ed, pos - 1) != '\n')
        pos--;
    return pos;
}

static size_t line_end(const editor_t *ed, size_t pos) {
    size_t len = editor_len(ed);
    while (pos < len && at(ed, pos) != '\n')
        pos++;
    return pos;
}

static size_t col_of(const editor_t *ed, size_t pos) {
    size_t col = 0;
    for (size_t p = line_start(ed, pos); p < pos;) {
        unit_t u = unit_at(ed, p, col);
        col += u.width;
        p += u.len;
    }
    return col;
}

// The position on the line starting at `start` closest to column `want` (not past it).
static size_t pos_at_col(const editor_t *ed, size_t start, size_t want) {
    size_t len = editor_len(ed), col = 0, p = start;
    while (p < len && at(ed, p) != '\n') {
        unit_t u = unit_at(ed, p, col);
        if (col + u.width > want)
            break;
        col += u.width;
        p += u.len;
    }
    return p;
}

// Newlines in [from, to). The status bar counts over the whole text every frame, so this runs
// memchr over each half of the gap buffer rather than looking at bytes one at a time.
static size_t count_lines(const editor_t *ed, size_t from, size_t to) {
    size_t n = 0;
    while (from < to) {
        bool before = from < ed->gap;
        size_t end = before && to > ed->gap ? ed->gap : to;
        const char *p = ed->buf + (before ? from : from + (ed->gap_end - ed->gap));
        const char *stop = p + (end - from);
        for (const char *q; (q = memchr(p, '\n', (size_t)(stop - p))) != NULL; p = q + 1)
            n++;
        from = end;
    }
    return n;
}

static int text_rows(const editor_t *ed) {
    return ed->scr.rows - 2;
}

// --- setup ---------------------------------------------------------------------------------------

static void begin_frame(void *ctx);
static size_t render_row(void *ctx, int r, char *out);
static void cursor_at(void *ctx, int *row, int *col, bool *shown);

static void changed(editor_t *ed) {
    screen_changed(&ed->scr);
}

void editor_message(editor_t *ed, const char *msg) {
    snprintf(ed->msg, sizeof(ed->msg), "%s", msg);
    changed(ed);
}

void editor_invalidate(editor_t *ed) {
    screen_invalidate(&ed->scr);
}

char *editor_open(editor_t *ed, const char *name, size_t len, size_t limit) {
    memset(ed, 0, sizeof(*ed));
    size_t nl = strlen(name);               // a long path: its end, where the file name is
    if (nl < sizeof(ed->name))
        snprintf(ed->name, sizeof(ed->name), "%s", name);
    else
        snprintf(ed->name, sizeof(ed->name), "...%s", name + nl - (sizeof(ed->name) - 4));
    screen_init(&ed->scr, begin_frame, render_row, cursor_at, ed);
    ed->limit = limit > EDITOR_MAX_TEXT ? EDITOR_MAX_TEXT : limit;
    if (len > ed->limit)
        return NULL;
    size_t cap = ed->limit;                 // all of it at once: see reserve()
    ed->buf = heap_alloc(HEAP_EDIT, cap ? cap : 1);
    if (!ed->buf)
        return NULL;
    ed->cap = cap;
    ed->gap = len;                          // the caller's bytes, until editor_opened()
    ed->gap_end = cap;
    return ed->buf;
}

void editor_opened(editor_t *ed) {
    // CRLF file: every LF has a CR before it (and there's at least one). Then the CRs go, in
    // place, and come back when saving.
    char *t = ed->buf;
    size_t len = ed->gap, lf = 0, crlf = 0;
    for (size_t i = 0; i < len; i++)
        if (t[i] == '\n') {
            lf++;
            crlf += i > 0 && t[i - 1] == '\r';
        }
    ed->crlf = lf > 0 && crlf == lf;
    if (ed->crlf) {
        size_t o = 0;
        for (size_t i = 0; i < len; i++)
            if (!(t[i] == '\r' && i + 1 < len && t[i + 1] == '\n'))
                t[o++] = t[i];
        ed->gap = o;
    }
    snprintf(ed->msg, sizeof(ed->msg), "Ctrl-S save, Ctrl-Q quit, Ctrl-F find, Ctrl-L redraw");
    editor_invalidate(ed);
}

bool editor_init(editor_t *ed, const char *name, const char *text, size_t len) {
    char *fill = editor_open(ed, name, len, EDITOR_MAX_TEXT);
    if (!fill)
        return false;
    memcpy(fill, text, len);
    editor_opened(ed);
    return true;
}

void editor_free(editor_t *ed) {
    heap_release(HEAP_EDIT, ed->buf);
    ed->buf = NULL;
    ed->cap = ed->gap = ed->gap_end = 0;
}

void editor_resize(editor_t *ed, int rows, int cols) {
    screen_resize(&ed->scr, rows, cols);
}

void editor_saved(editor_t *ed, bool ok, const char *msg) {
    if (ok)
        ed->modified = false;
    editor_message(ed, msg);
}

void editor_set_log_waiting(editor_t *ed, int n) {
    if (n != ed->log_waiting) {
        ed->log_waiting = n;
        changed(ed);
    }
}

// --- saving --------------------------------------------------------------------------------------

size_t editor_saved_len(const editor_t *ed) {
    size_t len = editor_len(ed);
    return len + (ed->crlf ? count_lines(ed, 0, len) : 0);
}

size_t editor_export(const editor_t *ed, size_t *pos, char *out, size_t cap) {
    size_t len = editor_len(ed), n = 0;
    while (*pos < len && n < cap) {
        char c = editor_at(ed, *pos);
        if (c == '\n' && ed->crlf) {
            if (n + 2 > cap)
                break;
            out[n++] = '\r';
        }
        out[n++] = c;
        (*pos)++;
    }
    return n;
}

// --- keys ----------------------------------------------------------------------------------------

static void line_up(editor_t *ed) {
    size_t s = line_start(ed, ed->cursor);
    if (s > 0)
        ed->cursor = pos_at_col(ed, line_start(ed, s - 1), ed->want_col);
}

static void line_down(editor_t *ed) {
    size_t e = line_end(ed, ed->cursor);
    if (e < editor_len(ed))
        ed->cursor = pos_at_col(ed, e + 1, ed->want_col);
}

// A word is letters, digits, '_' and anything non-ASCII (so every byte of a UTF-8 character,
// marks included, and the move stays on character boundaries); the rest, newlines included,
// separates words. Right goes to the end of the next word, left to the start of the previous
// one, as in Emacs and readline.
static bool word_byte(unsigned char c) {
    return c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '_';
}

static void word_right(editor_t *ed) {
    size_t len = editor_len(ed), p = ed->cursor;
    while (p < len && !word_byte(at(ed, p)))
        p++;
    while (p < len && word_byte(at(ed, p)))
        p++;
    ed->cursor = p;
}

static void word_left(editor_t *ed) {
    size_t p = ed->cursor;
    while (p > 0 && !word_byte(at(ed, p - 1)))
        p--;
    while (p > 0 && word_byte(at(ed, p - 1)))
        p--;
    ed->cursor = p;
}

static void find_next(editor_t *ed) {
    const char *q = ed->find_len ? ed->find : ed->last_find;
    size_t qn = strlen(q), len = editor_len(ed);
    if (!qn) {
        editor_message(ed, "");
        return;
    }
    if (q != ed->last_find)
        memcpy(ed->last_find, q, qn + 1);
    // From just after the cursor, wrapping around to the start.
    size_t start = next_pos(ed, ed->cursor);
    for (size_t k = 0; k < len; k++) {
        size_t p = (start + k) % len, i = 0;
        while (i < qn && p + i < len && at(ed, p + i) == (unsigned char)ed->last_find[i])
            i++;
        if (i == qn) {
            ed->cursor = p;
            ed->want_col = col_of(ed, p);
            editor_message(ed, p < start ? "search wrapped around" : "");
            return;
        }
    }
    editor_message(ed, "not found");
}

static editor_action_t find_key(editor_t *ed, int key) {
    if (key == '\r') {
        ed->finding = false;
        find_next(ed);
    } else if (key == CTRL('C') || key == CTRL('Q') || key == CTRL('G') || key == CTRL('F')) {
        ed->finding = false;
        editor_message(ed, "");
    } else if (key == 0x7f || key == '\b') {
        while (ed->find_len && ((unsigned char)ed->find[ed->find_len - 1] & 0xc0) == 0x80)
            ed->find_len--;                 // a whole UTF-8 character
        if (ed->find_len)
            ed->find_len--;
        ed->find[ed->find_len] = '\0';
    } else if (key >= 0x20 && key <= 0xff && key != 0x7f && ed->find_len < EDITOR_FIND_MAX) {
        ed->find[ed->find_len++] = (char)key;
        ed->find[ed->find_len] = '\0';
    }
    return EDITOR_NONE;
}

editor_action_t editor_key(editor_t *ed, int key) {
    changed(ed);
    if (ed->finding)
        return find_key(ed, key);
    bool armed = ed->quit_armed;
    ed->quit_armed = false;
    ed->msg[0] = '\0';
    bool keep_col = false;
    size_t len = editor_len(ed);

    switch (key) {
    case CTRL('Q'):
    case CTRL('X'):
        if (ed->modified && !armed) {
            ed->quit_armed = true;
            editor_message(ed, "unsaved changes: Ctrl-Q again to quit without saving");
            return EDITOR_NONE;
        }
        return EDITOR_QUIT;
    case CTRL('S'):
    case CTRL('O'):
        return EDITOR_SAVE;
    case CTRL('F'):
    case CTRL('W'):
        ed->finding = true;
        ed->find_len = 0;
        ed->find[0] = '\0';
        return EDITOR_NONE;
    case CTRL('L'):
        editor_invalidate(ed);
        return EDITOR_RESIZE;
    case CTRL('C'):
        editor_message(ed, "Ctrl-Q quits, Ctrl-S saves");
        return EDITOR_NONE;
    case '\r':
        insert(ed, "\n", 1);
        break;
    case '\t':
        insert(ed, "\t", 1);
        break;
    case 0x7f:
    case '\b':
        if (ed->cursor > 0) {
            size_t p = prev_pos(ed, ed->cursor);
            delete(ed, p, ed->cursor - p);
            ed->cursor = p;
        }
        break;
    case KEY_DELETE:
    case CTRL('D'):
        if (ed->cursor < len)
            delete(ed, ed->cursor, next_pos(ed, ed->cursor) - ed->cursor);
        break;
    case KEY_LEFT:
        ed->cursor = prev_pos(ed, ed->cursor);
        break;
    case KEY_RIGHT:
        ed->cursor = next_pos(ed, ed->cursor);
        break;
    case KEY_WORD_LEFT:
        word_left(ed);
        break;
    case KEY_WORD_RIGHT:
        word_right(ed);
        break;
    case KEY_CTRL_HOME:
        ed->cursor = 0;
        break;
    case KEY_CTRL_END:
        ed->cursor = len;
        break;
    case KEY_UP:
        line_up(ed);
        keep_col = true;
        break;
    case KEY_DOWN:
        line_down(ed);
        keep_col = true;
        break;
    case KEY_PGUP:
    case KEY_PGDN:
        for (int i = 0; i < text_rows(ed) - 1; i++)
            key == KEY_PGUP ? line_up(ed) : line_down(ed);
        keep_col = true;
        break;
    case KEY_HOME:
    case CTRL('A'):
        ed->cursor = line_start(ed, ed->cursor);
        break;
    case KEY_END:
    case CTRL('E'):
        ed->cursor = line_end(ed, ed->cursor);
        break;
    default:
        if (key >= 0x20 && key <= 0xff && key != 0x7f) {
            char c = (char)key;             // UTF-8 arrives a byte at a time
            insert(ed, &c, 1);
        }
        break;
    }
    if (!keep_col)
        ed->want_col = col_of(ed, ed->cursor);
    return EDITOR_NONE;
}

// --- screen --------------------------------------------------------------------------------------

// Keep the cursor on screen.
static void scroll(editor_t *ed) {
    int rows = text_rows(ed);
    size_t cols = (size_t)ed->scr.cols;
    if (ed->top > editor_len(ed))
        ed->top = line_start(ed, editor_len(ed));
    if (ed->cursor < ed->top) {
        ed->top = line_start(ed, ed->cursor);
    } else {
        size_t n = count_lines(ed, ed->top, ed->cursor);
        for (; n >= (size_t)rows; n--)
            ed->top = line_end(ed, ed->top) + 1;
    }
    size_t col = col_of(ed, ed->cursor), w = 1;
    if (ed->cursor < editor_len(ed) && at(ed, ed->cursor) != '\n') {
        w = unit_at(ed, ed->cursor, col).width;
        if (w > cols)
            w = 1;
    }
    if (col < ed->col_off)                  // back to the left: the start of the line if it fits
        ed->col_off = col + w <= cols ? 0 : col;
    else if (col + w > ed->col_off + cols)
        ed->col_off = col + w - cols;       // all of the character under the cursor
}

static void begin_frame(void *ctx) {
    scroll(ctx);
}

// One text row: the line starting at `line`, from column col_off (screen_unit_out() shows a wide
// character cut by either edge as spaces).
static size_t text_row(const editor_t *ed, size_t line, char *out, size_t n) {
    size_t len = editor_len(ed), col = 0, left = ed->col_off, right = left + (size_t)ed->scr.cols;
    for (size_t p = line; p < len && at(ed, p) != '\n' && col < right;) {
        unsigned char s[SCREEN_UNIT_MAX];
        unit_t u = screen_unit(s, unit_bytes_at(ed, p, s), col);
        n = screen_unit_out(out, n, s, u, col, left, right);
        col += u.width;
        p += u.len;
    }
    return n;
}

static size_t status_row(const editor_t *ed, char *out, size_t n) {
    char left[96], right[96];
    size_t line = count_lines(ed, 0, ed->cursor) + 1, lines = count_lines(ed, 0, editor_len(ed)) + 1;
    snprintf(left, sizeof(left), " %s%s", ed->name, ed->modified ? " [modified]" : "");
    int r = 0;
    if (ed->log_waiting)
        r = snprintf(right, sizeof(right), "%d log message%s held (dmesg) | ", ed->log_waiting,
                     ed->log_waiting == 1 ? "" : "s");
    snprintf(right + r, sizeof(right) - (size_t)r, "line %zu/%zu col %zu ", line, lines,
             col_of(ed, ed->cursor) + 1);
    size_t cols = (size_t)ed->scr.cols, rl = strlen(right);
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

static size_t message_row(const editor_t *ed, char *out, size_t n) {
    if (ed->finding) {
        n = screen_put(out, n, "Search: ", 8);
        return screen_put_text(out, n, ed->find, (size_t)ed->scr.cols - 8, NULL);
    }
    return screen_put_text(out, n, ed->msg, (size_t)ed->scr.cols, NULL);
}

// Screen row r (0-based): a text row (walking the lines from the top as the rows go), the status
// bar, or the message line.
static size_t render_row(void *ctx, int r, char *out) {
    editor_t *ed = ctx;
    size_t n = screen_row_start(r, out);
    if (r == 0) {
        ed->draw_line = ed->top;
        ed->draw_past_end = false;
    }
    if (r < text_rows(ed)) {
        if (ed->draw_past_end) {
            out[n++] = '~';
        } else {
            n = text_row(ed, ed->draw_line, out, n);
            size_t e = line_end(ed, ed->draw_line);
            if (e < editor_len(ed))
                ed->draw_line = e + 1;
            else
                ed->draw_past_end = true;
        }
    } else if (r == text_rows(ed)) {
        n = status_row(ed, out, n);
    } else {
        n = message_row(ed, out, n);
    }
    return n;
}

static void cursor_at(void *ctx, int *row, int *col, bool *shown) {
    editor_t *ed = ctx;
    *shown = true;
    if (ed->finding) {
        size_t used;
        screen_put_text(NULL, 0, ed->find, (size_t)ed->scr.cols - 8, &used);
        *row = ed->scr.rows;
        *col = 9 + (int)used;
        if (*col > ed->scr.cols)
            *col = ed->scr.cols;
    } else {
        *row = (int)count_lines(ed, ed->top, ed->cursor) + 1;
        *col = (int)(col_of(ed, ed->cursor) - ed->col_off) + 1;
    }
}

size_t editor_draw(editor_t *ed, char *out, size_t cap) {
    return screen_draw(&ed->scr, out, cap);
}

bool editor_drawn(const editor_t *ed) {
    return screen_drawn(&ed->scr);
}
