// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_PAGER_H
#define SOS_PAGER_H

#include <stdbool.h>
#include <stddef.h>

#include "edit/screen.h"

// The pager's model and screen, without the card or the shell: a text of any size read through a
// window, keys (keys.h codes) in, terminal output out (screen.h). Pure C, host tested; view.c
// connects it to a shell, a file on the card or a text in RAM (`more <file>`, edit_view()).
// A read that fails marks the pager failed: the status row says so until it quits, and no line
// total is kept from a count cut short.
//
// The text is `size` bytes read on demand with `read`, a window (PAGER_WINDOW bytes, or the whole
// text if smaller) of it held in RAM, so a file larger than memory can be read. Lines are
// separated by '\n'; a CR before it shows as '?', like any control character, so a CRLF file
// reads with a '?' at each line's end. Lines don't wrap: the view scrolls sideways (Left/Right).
// Line numbers are counted from the start as the view moves, and the total is counted (one pass
// over the text) the first time it is needed: the end key, or a search that wraps around.

#define PAGER_WINDOW   (16 * 1024)      // of the text in RAM at a time
#define PAGER_FIND_MAX 64

typedef struct {
    // Copy up to n bytes from off into buf; the count copied, fewer than asked only at the end of
    // the text, 0 on error.
    size_t (*read)(void *ctx, size_t off, char *buf, size_t n);
    void *ctx;
    size_t size;                // bytes in the text (so far, if growing)
    // The text grows while it is shown (pager_grow()): the window is allocated whole from the
    // start, as `size` says nothing about how large the text will get. Set it here, before
    // pager_init(); pager_set_growing() ends it, when the text is complete.
    bool growing;
} pager_source_t;

typedef enum {
    PAGER_NONE,         // nothing for the caller to do
    PAGER_QUIT,         // leave the pager
    PAGER_RESIZE,       // ask the terminal for its size (the answer goes to pager_resize)
} pager_action_t;

typedef struct {
    screen_t scr;
    pager_source_t src;

    char *win;                  // the window: bytes [win_off, win_off + win_len) of the text
    size_t win_cap, win_off, win_len;
    bool failed;                // a read failed: the message says so

    size_t top;                 // offset of the first line shown
    long top_line;              // its line number, from 0
    long lines;                 // lines in the text, or -1 until counted
    size_t end_top;             // `top` with the last line on the bottom row (SIZE_MAX: not known)
    long end_top_line;          // its line number (-1: not counted yet)
    int end_shown;              // lines on screen at end_top
    size_t col_off;             // first column shown (horizontal scrolling)
    bool growing;               // the text may still grow: no total, the end is the end so far
    bool follow;                // growing: stay on the end as text arrives (the F key)

    bool finding;               // the status row is a search prompt
    char find[PAGER_FIND_MAX + 1];
    size_t find_len;
    char last_find[PAGER_FIND_MAX + 1];

    char name[64];              // shown in the status row
    char msg[128];              // shown instead of the status until the next key
    int log_waiting;            // console log messages held back while paging

    // Drawing: where the row being rendered starts, and how many lines the frame has shown.
    size_t draw_pos;
    bool draw_past_end;
    int shown;
} pager_t;

// Start on `src`, with a window of `window` bytes (0: PAGER_WINDOW) or the whole text if it is
// smaller. `name` is shown in the status row. False if out of memory.
bool pager_init(pager_t *p, const char *name, pager_source_t src, size_t window);
void pager_free(pager_t *p);

// Handle one key: a byte or a KEY_* code (keys.h). Returns what the caller must do next.
pager_action_t pager_key(pager_t *p, int key);

// The terminal's size, from the cursor position report; clamped to screen.h's limits.
void pager_resize(pager_t *p, int rows, int cols);

void pager_message(pager_t *p, const char *msg);
// Console log messages waiting (log_pending()), shown in the status row.
void pager_set_log_waiting(pager_t *p, int n);

// A text that grows while it is shown (a pipeline's output, a log being written). The source
// is marked `growing` in pager_init(); the caller appends to the text and then calls
// pager_grow() with the new size (larger than before; a smaller one is ignored). The line count
// is extended, not redone; a view scrolled back doesn't move; a view on the end shows the new
// lines on the next frame, and so does one in follow mode (the F key: stay on the end as text
// arrives; any other key, except h, ? and Ctrl-L, leaves it). While growing, the status row shows
// the total as `...` and the end key goes to the end so far. pager_set_growing(p, false) says
// the text is complete: the total is shown from then on, and follow mode ends.
// Bytes already in the text must not change; only the end grows.
void pager_grow(pager_t *p, size_t size);
void pager_set_growing(pager_t *p, bool growing);

// Terminal output goes through the screen: screen_draw(&p->scr, ...) until screen_drawn().

// A source over a text in RAM: ctx points at the text. If the caller's block moves as the text
// grows (realloc), it sets p->src.ctx to the new block before the next pager call (the window
// holds copies of what was read, and the text before the end doesn't change). That is the
// re-pointing rule: ctx stays the pointer to the text, not to a pointer to it.
size_t pager_read_mem(void *ctx, size_t off, char *buf, size_t n);

#endif
