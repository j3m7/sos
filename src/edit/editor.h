// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_EDITOR_H
#define SOS_EDITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The full-screen text editor's model and screen, without the card or the shell: text in a gap
// buffer, keys (keys.h codes) in, terminal output out. Pure C, host tested; edit.c connects it to
// a shell and the card.

// Bytes of text at most. The heap (about 345 KB) is shared with SSH, TLS and Lua, so edit.c lowers
// the limit of an editor it opens to what the heap can spare then (editor_t.limit).
#define EDITOR_MAX_TEXT  (128 * 1024)
#define EDITOR_ROWS_MIN  4
#define EDITOR_ROWS_MAX  60
#define EDITOR_COLS_MIN  20
#define EDITOR_COLS_MAX  160
#define EDITOR_MAX_MARKS 2              // combining marks shown on one character; more show as '?'
// Most output one screen row can take: each column holds one character of up to 4 bytes with
// its marks. It must fit in what edit.c can send at once (2048 bytes, SSH's output per pass),
// which is what limits EDITOR_COLS_MAX.
#define EDITOR_ROW_BYTES (16 + EDITOR_COLS_MAX * 4 * (1 + EDITOR_MAX_MARKS))
#define EDITOR_FIND_MAX  64
#define EDITOR_TAB       8

typedef enum {
    EDITOR_NONE,        // nothing for the caller to do
    EDITOR_SAVE,        // save the text (editor_export), then call editor_saved()
    EDITOR_QUIT,        // leave the editor
    EDITOR_RESIZE,      // ask the terminal for its size (the answer goes to editor_resize)
} editor_action_t;

typedef struct {
    // Text: a gap buffer, bytes [0, gap) and [gap_end, cap) of buf.
    char *buf;
    size_t cap, gap, gap_end;
    size_t limit;               // the text may grow to this many bytes (at most EDITOR_MAX_TEXT)
    bool crlf;                  // the file had CRLF line ends: saved with them again

    size_t cursor;              // byte offset into the text
    size_t top;                 // offset of the first line on screen
    size_t want_col;            // column Up/Down aim for
    size_t col_off;             // first column shown (horizontal scrolling)
    int rows, cols;             // terminal size; the last two rows are the status and message

    bool modified;
    bool quit_armed;            // Ctrl-Q with unsaved changes: a second one quits

    bool finding;               // the message line is a search prompt
    char find[EDITOR_FIND_MAX + 1];
    size_t find_len;
    char last_find[EDITOR_FIND_MAX + 1];

    char name[64];              // shown in the status bar
    char msg[96];               // message line
    int log_waiting;            // console log messages held back while editing

    // Screen state: what each row shows now (hash), and how far the current frame has got.
    uint32_t drawn[EDITOR_ROWS_MAX];
    bool row_valid[EDITOR_ROWS_MAX];
    int draw_row;               // next row to compare/draw; rows = only the cursor is left
    size_t draw_line;           // offset of the text line for draw_row (if it's a text row)
    bool draw_past_end;         // draw_row is past the last line of text
    bool drawing;               // a frame is under way
    bool dirty;                 // the screen needs a frame
    bool clear;                 // the next frame starts by clearing the whole screen
    char row[EDITOR_ROW_BYTES]; // scratch for one row (kept off the stack)
} editor_t;

// Start with `len` bytes of file contents (copied; CRLF line ends are stripped and remembered).
// `name` is shown in the status bar. False if out of memory or over EDITOR_MAX_TEXT.
bool editor_init(editor_t *ed, const char *name, const char *text, size_t len);
// The same in two steps, so a file can be read straight into the editor without a second copy
// of it: editor_open() gives room for `len` bytes, the caller fills it (all of it), then
// editor_opened() looks at the text (CRLF) and the editor is ready. `limit` is the most text
// this editor may hold, at most EDITOR_MAX_TEXT. NULL if `len` is over the limit or memory is
// short; editor_free() is safe after either outcome, and nothing else may be called in between.
char *editor_open(editor_t *ed, const char *name, size_t len, size_t limit);
void editor_opened(editor_t *ed);
void editor_free(editor_t *ed);

// Handle one key: a byte or a KEY_* code (keys.h). Returns what the caller must do next.
editor_action_t editor_key(editor_t *ed, int key);

// The terminal's size, from the cursor position report; clamped to the limits above.
void editor_resize(editor_t *ed, int rows, int cols);

// After an EDITOR_SAVE: whether it worked, and a message for the message line.
void editor_saved(editor_t *ed, bool ok, const char *msg);
void editor_message(editor_t *ed, const char *msg);
// Console log messages waiting (log_pending()), shown in the status bar.
void editor_set_log_waiting(editor_t *ed, int n);

// Text length as saved (with CRLF restored), and the bytes from *pos on: fills up to `cap` bytes
// of `out` (at least 2, room for a CRLF), advances *pos, returns the count (0 at the end).
size_t editor_saved_len(const editor_t *ed);
size_t editor_export(const editor_t *ed, size_t *pos, char *out, size_t cap);

// Terminal output. editor_draw() fills `out` with up to `cap` bytes of the next screen update,
// whole rows only, skipping rows that already show the right thing, and positions the cursor
// when it has finished. Call it until editor_drawn() is true; any change in between restarts the
// frame. Returns 0 when `cap` can't take the next row: call again with more room.
size_t editor_draw(editor_t *ed, char *out, size_t cap);
bool editor_drawn(const editor_t *ed);
// Redraw everything (Ctrl-L, a new size, or the screen was disturbed).
void editor_invalidate(editor_t *ed);

// Text access, for tests and saving.
size_t editor_len(const editor_t *ed);
char editor_at(const editor_t *ed, size_t i);

#endif
