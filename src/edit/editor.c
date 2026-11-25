// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The full-screen editor's model and screen (see editor.h). Depends only on keys.h, termtext.c
// and the C library, so it builds on the host for unit tests.
//
// Text is bytes in a gap buffer; the cursor moves by UTF-8 character. Each character takes one
// column (a tab up to the next multiple of EDITOR_TAB), so wide CJK characters misalign the rest
// of their line. Anything a terminal could act on, or that would hide what the bytes are
// (controls, invalid UTF-8, bidi and invisible characters: termtext_hidden()), shows as '?'.
// Lines don't wrap: the view scrolls sideways to follow the cursor. The terminal's autowrap should
// be off (edit.c does that), so a wide character can't push a row onto the next one.

#include "edit/editor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell/keys.h"
#include "shell/termtext.h"

#define BAD_CP 0xFFFFFFFFu
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

// Room for n more bytes, growing the buffer (up to EDITOR_MAX_TEXT) if needed.
static bool reserve(editor_t *ed, size_t n) {
    if (ed->gap_end - ed->gap >= n)
        return true;
    size_t len = editor_len(ed);
    if (len + n > EDITOR_MAX_TEXT)
        return false;
    size_t cap = ed->cap ? ed->cap : 1024;
    while (cap - len < n)
        cap *= 2;
    if (cap > EDITOR_MAX_TEXT)
        cap = EDITOR_MAX_TEXT;              // still enough: len + n <= EDITOR_MAX_TEXT
    char *b = realloc(ed->buf, cap);
    if (!b)
        return false;
    size_t tail = ed->cap - ed->gap_end;
    memmove(b + cap - tail, b + ed->gap_end, tail);
    ed->buf = b;
    ed->gap_end = cap - tail;
    ed->cap = cap;
    return true;
}

static bool insert(editor_t *ed, const char *s, size_t n) {
    if (!reserve(ed, n)) {
        editor_message(ed, "the text can't grow past 32 KB");
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

// The UTF-8 character at pos: its length (1 for a byte that doesn't start a valid one) and code
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

static size_t decode(const editor_t *ed, size_t pos, uint32_t *cp) {
    unsigned char s[4];
    size_t avail = editor_len(ed) - pos;
    if (avail > 4)
        avail = 4;
    for (size_t i = 0; i < avail; i++)
        s[i] = at(ed, pos + i);
    return decode_bytes(s, avail, cp);
}

static size_t next_pos(const editor_t *ed, size_t pos) {
    uint32_t cp;
    return pos < editor_len(ed) ? pos + decode(ed, pos, &cp) : pos;
}

static size_t prev_pos(const editor_t *ed, size_t pos) {
    if (pos == 0)
        return 0;
    size_t p = pos - 1;
    for (int back = 0; back < 3 && p > 0 && (at(ed, p) & 0xc0) == 0x80; back++)
        p--;
    uint32_t cp;
    return decode(ed, p, &cp) == pos - p ? p : pos - 1;
}

// Shown as itself (else as '?').
static bool shows(uint32_t cp) {
    return (cp >= 0x20 && cp < 0x7f) || (cp >= 0xa0 && cp != BAD_CP && !termtext_hidden(cp));
}

static size_t width(uint32_t cp, size_t col) {
    return cp == '\t' ? EDITOR_TAB - col % EDITOR_TAB : 1;
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
        uint32_t cp;
        size_t l = decode(ed, p, &cp);
        col += width(cp, col);
        p += l;
    }
    return col;
}

// The position on the line starting at `start` closest to column `want` (not past it).
static size_t pos_at_col(const editor_t *ed, size_t start, size_t want) {
    size_t len = editor_len(ed), col = 0, p = start;
    while (p < len && at(ed, p) != '\n') {
        uint32_t cp;
        size_t l = decode(ed, p, &cp);
        size_t w = width(cp, col);
        if (col + w > want)
            break;
        col += w;
        p += l;
    }
    return p;
}

static size_t count_lines(const editor_t *ed, size_t from, size_t to) {
    size_t n = 0;
    for (size_t p = from; p < to; p++)
        n += at(ed, p) == '\n';
    return n;
}

static int text_rows(const editor_t *ed) {
    return ed->rows - 2;
}

// --- setup ---------------------------------------------------------------------------------------

static void changed(editor_t *ed) {
    ed->dirty = true;
    ed->drawing = false;                    // restart the frame
}

void editor_message(editor_t *ed, const char *msg) {
    snprintf(ed->msg, sizeof(ed->msg), "%s", msg);
    changed(ed);
}

void editor_invalidate(editor_t *ed) {
    ed->clear = true;
    memset(ed->row_valid, 0, sizeof(ed->row_valid));
    changed(ed);
}

bool editor_init(editor_t *ed, const char *name, const char *text, size_t len) {
    memset(ed, 0, sizeof(*ed));
    size_t nl = strlen(name);               // a long path: its end, where the file name is
    if (nl < sizeof(ed->name))
        snprintf(ed->name, sizeof(ed->name), "%s", name);
    else
        snprintf(ed->name, sizeof(ed->name), "...%s", name + nl - (sizeof(ed->name) - 4));
    ed->rows = 24;
    ed->cols = 80;

    // CRLF file: every LF has a CR before it (and there's at least one).
    size_t lf = 0, crlf = 0;
    for (size_t i = 0; i < len; i++)
        if (text[i] == '\n') {
            lf++;
            crlf += i > 0 && text[i - 1] == '\r';
        }
    ed->crlf = lf > 0 && crlf == lf;
    size_t n = len - (ed->crlf ? crlf : 0);
    if (n > EDITOR_MAX_TEXT)
        return false;

    size_t cap = n + 1024 > EDITOR_MAX_TEXT ? EDITOR_MAX_TEXT : n + 1024;
    ed->buf = malloc(cap ? cap : 1);
    if (!ed->buf)
        return false;
    ed->cap = cap;
    size_t o = 0;
    for (size_t i = 0; i < len; i++)
        if (!(ed->crlf && text[i] == '\r' && i + 1 < len && text[i + 1] == '\n'))
            ed->buf[o++] = text[i];
    ed->gap = o;
    ed->gap_end = cap;
    snprintf(ed->msg, sizeof(ed->msg), "Ctrl-S save, Ctrl-Q quit, Ctrl-F find, Ctrl-L redraw");
    editor_invalidate(ed);
    return true;
}

void editor_free(editor_t *ed) {
    free(ed->buf);
    ed->buf = NULL;
    ed->cap = ed->gap = ed->gap_end = 0;
}

void editor_resize(editor_t *ed, int rows, int cols) {
    ed->rows = rows < EDITOR_ROWS_MIN ? EDITOR_ROWS_MIN : rows > EDITOR_ROWS_MAX ? EDITOR_ROWS_MAX : rows;
    ed->cols = cols < EDITOR_COLS_MIN ? EDITOR_COLS_MIN : cols > EDITOR_COLS_MAX ? EDITOR_COLS_MAX : cols;
    editor_invalidate(ed);
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
    if (ed->top > editor_len(ed))
        ed->top = line_start(ed, editor_len(ed));
    if (ed->cursor < ed->top) {
        ed->top = line_start(ed, ed->cursor);
    } else {
        size_t n = count_lines(ed, ed->top, ed->cursor);
        for (; n >= (size_t)rows; n--)
            ed->top = line_end(ed, ed->top) + 1;
    }
    size_t col = col_of(ed, ed->cursor);
    if (col < ed->col_off)
        ed->col_off = col;
    else if (col >= ed->col_off + (size_t)ed->cols)
        ed->col_off = col - (size_t)ed->cols + 1;
}

static size_t put(char *out, size_t n, const char *s, size_t len) {
    memcpy(out + n, s, len);
    return n + len;
}

// Up to `cols` columns of a C string, filtered like text. With out NULL, only counts columns.
static size_t put_text(char *out, size_t n, const char *s, size_t cols, size_t *used) {
    size_t len = strlen(s), col = 0;
    for (size_t p = 0; p < len && col < cols;) {
        uint32_t cp;
        size_t l = decode_bytes((const unsigned char *)s + p, len - p, &cp);
        if (!out)
            ;
        else if (shows(cp))
            n = put(out, n, s + p, l);
        else
            out[n++] = '?';
        col++;
        p += l;
    }
    if (used)
        *used = col;
    return n;
}

// One text row: the line starting at `line`, from column col_off.
static size_t text_row(const editor_t *ed, size_t line, char *out, size_t n) {
    size_t len = editor_len(ed), col = 0, left = ed->col_off, right = left + (size_t)ed->cols;
    for (size_t p = line; p < len && at(ed, p) != '\n' && col < right;) {
        uint32_t cp;
        size_t l = decode(ed, p, &cp);
        size_t w = width(cp, col);
        if (col + w > left) {
            if (cp == '\t') {
                size_t from = col < left ? left : col, to = col + w > right ? right : col + w;
                for (size_t i = from; i < to; i++)
                    out[n++] = ' ';
            } else if (shows(cp)) {
                for (size_t i = 0; i < l; i++)
                    out[n++] = editor_at(ed, p + i);
            } else {
                out[n++] = '?';
            }
        }
        col += w;
        p += l;
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
    size_t cols = (size_t)ed->cols, rl = strlen(right);
    if (rl > cols)
        rl = cols;
    n = put(out, n, "\x1b[7m", 4);
    size_t used;
    n = put_text(out, n, left, cols - rl, &used);
    for (; used < cols - rl; used++)
        out[n++] = ' ';
    n = put(out, n, right + strlen(right) - rl, rl);
    return put(out, n, "\x1b[m", 3);
}

static size_t message_row(const editor_t *ed, char *out, size_t n) {
    if (ed->finding) {
        n = put(out, n, "Search: ", 8);
        return put_text(out, n, ed->find, (size_t)ed->cols - 8, NULL);
    }
    return put_text(out, n, ed->msg, (size_t)ed->cols, NULL);
}

// Render screen row r (0-based) into out (EDITOR_ROW_BYTES): position, erase the row, contents.
// Erasing first: after a character in the last column the cursor stays on it, so an erase
// after the contents would remove that character from a full row.
static size_t render_row(const editor_t *ed, int r, char *out) {
    size_t n = (size_t)snprintf(out, 16, "\x1b[%d;1H\x1b[K", r + 1);
    if (r < text_rows(ed)) {
        if (ed->draw_past_end)
            out[n++] = '~';
        else
            n = text_row(ed, ed->draw_line, out, n);
    } else if (r == text_rows(ed)) {
        n = status_row(ed, out, n);
    } else {
        n = message_row(ed, out, n);
    }
    return n;
}

static uint32_t hash(const char *s, size_t n) {
    uint32_t h = 2166136261u;               // FNV-1a
    for (size_t i = 0; i < n; i++)
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

bool editor_drawn(const editor_t *ed) {
    return !ed->dirty;
}

size_t editor_draw(editor_t *ed, char *out, size_t cap) {
    size_t n = 0;
    if (!ed->dirty)
        return 0;
    if (!ed->drawing) {
        if (cap < 16)
            return 0;
        scroll(ed);
        ed->drawing = true;
        ed->draw_row = 0;
        ed->draw_line = ed->top;
        ed->draw_past_end = false;
        n = put(out, n, "\x1b[?25l", 6);    // hide the cursor while drawing
        if (ed->clear) {
            n = put(out, n, "\x1b[2J", 4);
            ed->clear = false;
        }
    }
    char *row = ed->row;
    while (ed->draw_row < ed->rows) {
        int r = ed->draw_row;
        size_t len = render_row(ed, r, row);
        uint32_t h = hash(row, len);
        if (!ed->row_valid[r] || ed->drawn[r] != h) {
            if (n + len > cap)
                return n;                   // the rest on the next call
            n = put(out, n, row, len);
            ed->drawn[r] = h;
            ed->row_valid[r] = true;
        }
        if (r < text_rows(ed) && !ed->draw_past_end) {
            size_t e = line_end(ed, ed->draw_line);
            if (e < editor_len(ed))
                ed->draw_line = e + 1;
            else
                ed->draw_past_end = true;
        }
        ed->draw_row++;
    }
    // Finally the cursor.
    char tail[32];
    int row_no, col_no;
    if (ed->finding) {
        size_t used;
        put_text(NULL, 0, ed->find, (size_t)ed->cols - 8, &used);
        row_no = ed->rows;
        col_no = 9 + (int)used;
        if (col_no > ed->cols)
            col_no = ed->cols;
    } else {
        row_no = (int)count_lines(ed, ed->top, ed->cursor) + 1;
        col_no = (int)(col_of(ed, ed->cursor) - ed->col_off) + 1;
    }
    size_t tl = (size_t)snprintf(tail, sizeof(tail), "\x1b[%d;%dH\x1b[?25h", row_no, col_no);
    if (n + tl > cap)
        return n;
    n = put(out, n, tail, tl);
    ed->drawing = false;
    ed->dirty = false;
    return n;
}
