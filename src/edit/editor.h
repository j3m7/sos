// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_EDITOR_H
#define SOS_EDITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "edit/screen.h"

// The full-screen text editor's model and screen, without the card or the shell: text in a gap
// buffer, keys (keys.h codes) in, terminal output out (through screen.h). Pure C, host tested;
// edit.c connects it to a shell and the card.

// Bytes of text at most. The heap (about 345 KB) is shared with SSH, TLS and Lua, so edit.c lowers
// the limit of an editor it opens to what the heap can spare then (editor_t.limit).
#define EDITOR_MAX_TEXT  (128 * 1024)
// The screen's limits (screen.h), under the editor's names.
#define EDITOR_ROWS_MIN  SCREEN_ROWS_MIN
#define EDITOR_ROWS_MAX  SCREEN_ROWS_MAX
#define EDITOR_COLS_MIN  SCREEN_COLS_MIN
#define EDITOR_COLS_MAX  SCREEN_COLS_MAX
#define EDITOR_MAX_MARKS SCREEN_MAX_MARKS
#define EDITOR_ROW_BYTES SCREEN_ROW_BYTES
#define EDITOR_FIND_MAX  64
#define EDITOR_TAB       SCREEN_TAB

typedef enum {
    EDITOR_NONE,        // nothing for the caller to do
    EDITOR_SAVE,        // save the text (editor_export), then call editor_saved()
    EDITOR_QUIT,        // leave the editor
    EDITOR_RESIZE,      // ask the terminal for its size (the answer goes to editor_resize)
} editor_action_t;

typedef struct {
    screen_t scr;               // the terminal's size, and what each row shows now

    // Text: a gap buffer, bytes [0, gap) and [gap_end, cap) of buf.
    char *buf;
    size_t cap, gap, gap_end;
    size_t limit;               // the text may grow to this many bytes (at most EDITOR_MAX_TEXT)
    bool crlf;                  // the file had CRLF line ends: saved with them again

    size_t cursor;              // byte offset into the text
    size_t top;                 // offset of the first line on screen
    size_t want_col;            // column Up/Down aim for
    size_t col_off;             // first column shown (horizontal scrolling)

    bool modified;
    bool quit_armed;            // Ctrl-Q with unsaved changes: a second one quits

    bool finding;               // the message line is a search prompt
    char find[EDITOR_FIND_MAX + 1];
    size_t find_len;
    char last_find[EDITOR_FIND_MAX + 1];

    char name[64];              // shown in the status bar
    char msg[96];               // message line
    int log_waiting;            // console log messages held back while editing

    // Drawing: the text line for the row being rendered, or past the last line.
    size_t draw_line;
    bool draw_past_end;
} editor_t;

// Start with `len` bytes of file contents (copied; CRLF line ends are stripped and remembered).
// `name` is shown in the status bar. False if out of memory or over EDITOR_MAX_TEXT.
bool editor_init(editor_t *ed, const char *name, const char *text, size_t len);
// The same in two steps, so a file can be read straight into the editor without a second copy
// of it: editor_open() gives room for `len` bytes, the caller fills it (all of it), then
// editor_opened() looks at the text (CRLF) and the editor is ready. `limit` is the most text
// this editor may hold, at most EDITOR_MAX_TEXT, and is allocated whole at once. NULL if `len`
// (the file's bytes as stored, CRs included) is over the limit or memory is short; editor_free()
// is safe after either outcome, and nothing else may be called in between.
char *editor_open(editor_t *ed, const char *name, size_t len, size_t limit);
void editor_opened(editor_t *ed);
void editor_free(editor_t *ed);

// Handle one key: a byte or a KEY_* code (keys.h). Returns what the caller must do next.
editor_action_t editor_key(editor_t *ed, int key);

// The terminal's size, from the cursor position report; clamped to the screen's limits.
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

// Terminal output: screen_draw() and screen_drawn() on ed->scr (screen.h), under the editor's
// names.
size_t editor_draw(editor_t *ed, char *out, size_t cap);
bool editor_drawn(const editor_t *ed);
// Redraw everything (Ctrl-L, a new size, or the screen was disturbed).
void editor_invalidate(editor_t *ed);

// Text access, for tests and saving.
size_t editor_len(const editor_t *ed);
char editor_at(const editor_t *ed, size_t i);

#endif
