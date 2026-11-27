// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_FSTERM_H
#define SOS_FSTERM_H

#include <stdbool.h>
#include <stdint.h>

#include "edit/screen.h"
#include "shell/shell.h"

// The terminal side of a full-screen program run as a raw-mode shell job (shell_run_raw): the
// editor and the pager share it. It puts the terminal on its alternate screen with autowrap off
// (so a wide character can't push a row onto the next one), asks the terminal for its size by
// moving the cursor to the far corner and asking where it ended up (ESC [ 6 n; the answer comes
// as KEY_CURSOR_POS), sends each frame as the channel's free space allows, counts the console's
// held log messages, and gives the screen back at the end.

typedef struct {
    bool want_size;                 // send the size query when there's room
    bool size_asked;                // ... sent: the next cursor position report is the answer
    uint64_t size_wait_until;       // no drawing before then (unless the answer comes)
    uint64_t size_expires;          // size_asked ends then (Ctrl+F3 sends the same bytes)
    uint64_t next_log_check;
    char out[2048];                 // one write's worth of output (SSH takes 2048 a pass)
} fsterm_t;

// On the way in: the alternate screen, autowrap off; the size is asked at the first step.
void fsterm_enter(shell_t *sh, fsterm_t *t);
// Ask the terminal for its size (again).
void fsterm_ask_size(fsterm_t *t);
// A key for the job: true if it was a cursor position report (the program ignores it), with
// *rows and *cols set if it answers the size query, 0 otherwise.
bool fsterm_size_key(shell_t *sh, fsterm_t *t, int key, int *rows, int *cols);
// Once per pass, before drawing: sends the size query when there's room, and on the console counts
// the held log messages into *log_waiting (unchanged elsewhere, or when it isn't time to count).
// False while the first frame waits for the terminal's size.
bool fsterm_step(shell_t *sh, fsterm_t *t, int *log_waiting);
// As much of the next frame as the channel takes without waiting.
void fsterm_send(shell_t *sh, fsterm_t *t, screen_t *scr);
// On the way out: a row cut short goes out to its end (so the terminal never sees a cut
// sequence), the bottom row is erased (the prompt goes there on a terminal without an alternate
// screen), the cursor is shown again, autowrap on, back to the shell's screen. With `cancel` (the
// session is gone) only on the console, which cancels a job when it locks itself with the
// terminal possibly still there.
void fsterm_leave(shell_t *sh, const screen_t *scr, bool cancel);

#endif
