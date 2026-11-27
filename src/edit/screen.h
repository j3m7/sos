// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_SCREEN_H
#define SOS_SCREEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// What the full-screen programs (the editor, the pager) share of the screen, without the shell:
// how a character is laid out, and a frame driver that sends only the rows that changed, in
// pieces when the channel has less room than a row. Pure C, host tested.

// Screen size: a bigger terminal uses only this much of itself. The cost of the maximum is the
// row scratch below (about 4.8 KB) and a hash per row.
#define SCREEN_ROWS_MIN  4
#define SCREEN_ROWS_MAX  200
#define SCREEN_COLS_MIN  20
#define SCREEN_COLS_MAX  400
#define SCREEN_MAX_MARKS 2              // combining marks shown on one character; more show as '?'
#define SCREEN_TAB       8
// The bytes one column can hold: a character of up to 4 bytes with its marks.
#define SCREEN_UNIT_MAX  (4 * (1 + SCREEN_MAX_MARKS))
// Most output one screen row can take: positioning, then one unit per column. A row goes out in
// pieces if the channel has less room than this.
#define SCREEN_ROW_BYTES (16 + SCREEN_COLS_MAX * SCREEN_UNIT_MAX)

// --- layout --------------------------------------------------------------------------------------

// What the screen shows for one character: its bytes, the combining marks attached to it (at most
// SCREEN_MAX_MARKS), how many columns it takes, and whether its bytes go out as they are
// (`glyph`); if not, it shows as '?' in one column, or as spaces for a tab. Everything that lays
// out a line goes through screen_unit(), so a line's columns, the cursor and what's drawn agree.
// A mark that can't be attached (at the start of a line, after a tab or a '?', or beyond the
// limit) is a unit of its own and shows as '?', so no bytes of the text are hidden. A character
// takes the columns charwidth() says, a tab goes up to the next multiple of SCREEN_TAB, and
// anything a terminal could act on or that would hide what the bytes are (controls, invalid
// UTF-8, bidi and invisible characters: termtext_hidden()) shows as '?' in one column.
typedef struct {
    size_t len;         // bytes, marks included
    size_t width;       // columns
    bool glyph;         // send the bytes (else '?', or spaces for a tab)
    bool tab;
} unit_t;

// The unit at s (at most `avail` bytes to look at), starting at column col.
unit_t screen_unit(const unsigned char *s, size_t avail, size_t col);

// Output helpers: append to out at n, return the new n.
size_t screen_put(char *out, size_t n, const char *s, size_t len);
// The unit u at s, which starts at column col, as shown in the window of columns [left, right):
// nothing if outside, spaces for a tab or a wide character cut by an edge, else its bytes or '?'.
size_t screen_unit_out(char *out, size_t n, const unsigned char *s, unit_t u, size_t col,
                       size_t left, size_t right);
// Up to `cols` columns of a C string, laid out like text (a tab shows as '?'). With out NULL, only
// counts the columns, into *used.
size_t screen_put_text(char *out, size_t n, const char *s, size_t cols, size_t *used);
// The line at s (up to len bytes, or a '\n'), from column col_off, over `cols` columns.
size_t screen_text(char *out, size_t n, const unsigned char *s, size_t len, size_t col_off, size_t cols);
// The start of a row: position the cursor on row r (0-based), erase the row. Erasing first:
// after a character in the last column the cursor stays on it, so an erase after the contents
// would remove that character from a full row.
size_t screen_row_start(int r, char *out);

// --- frames --------------------------------------------------------------------------------------

typedef struct screen screen_t;

// Called when a frame starts, before any row: put the view right (scroll to the cursor, ...).
typedef void (*screen_begin_fn)(void *ctx);
// Render row r (0-based) into out (SCREEN_ROW_BYTES): screen_row_start(), then the contents.
// Rows are asked for in order, 0 to rows - 1, once each per frame. Returns the length.
typedef size_t (*screen_row_fn)(void *ctx, int r, char *out);
// Where the cursor goes once the frame is complete (1-based), and whether it is shown.
typedef void (*screen_cursor_fn)(void *ctx, int *row, int *col, bool *shown);

struct screen {
    int rows, cols;             // terminal size
    screen_begin_fn begin;
    screen_row_fn render;
    screen_cursor_fn cursor;
    void *ctx;
    // What each row shows now (a hash), and how far the current frame has got.
    uint32_t drawn[SCREEN_ROWS_MAX];
    bool row_valid[SCREEN_ROWS_MAX];
    int draw_row;               // next row to compare/draw; rows = only the cursor is left
    bool drawing;               // a frame is under way
    bool dirty;                 // the screen needs a frame
    bool clear;                 // the next frame starts by clearing the whole screen
    char row[SCREEN_ROW_BYTES]; // scratch for one row (kept off the stack)
    size_t row_len, row_sent;   // the rendered row being sent, and how much of it has gone
};

// 24x80 until screen_resize(); the first frame clears the screen.
void screen_init(screen_t *s, screen_begin_fn begin, screen_row_fn render, screen_cursor_fn cursor,
                 void *ctx);
// The terminal's size, clamped to the limits above; redraws everything.
void screen_resize(screen_t *s, int rows, int cols);
// Something shown may have changed: the next screen_draw() starts a frame (a frame under way
// restarts).
void screen_changed(screen_t *s);
// Redraw everything (Ctrl-L, or the screen was disturbed).
void screen_invalidate(screen_t *s);

// Terminal output. screen_draw() fills `out` with up to `cap` bytes of the next screen update, a
// row at a time (in pieces, if `cap` has less room than a row), skipping rows that already show
// the right thing, and positions the cursor when it has finished. Call it until screen_drawn() is
// true; a screen_changed() in between restarts the frame (a row sent in part is first sent to
// its end, so the terminal never sees a cut sequence). Returns 0 only when `cap` (at least 16 to
// start a frame, about 32 to end one) is too small to make progress: call again with more room.
size_t screen_draw(screen_t *s, char *out, size_t cap);
bool screen_drawn(const screen_t *s);

#endif
