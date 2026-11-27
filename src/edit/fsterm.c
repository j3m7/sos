// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The terminal side of a full-screen job (see fsterm.h). It uses the terminal's alternate screen
// (xterm, and most terminals since), so the shell's output is back as it was afterwards.

#include "edit/fsterm.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "core/log.h"
#include "shell/keys.h"

#define SIZE_WAIT_US    500000      // first frame: wait this long for the terminal's size
#define SIZE_ANSWER_US  3000000     // a cursor position report later than this isn't the answer
#define LOG_CHECK_US    1000000     // how often to count held log messages (console)

static void put(shell_t *sh, const char *s) {
    sh->io->write(sh->ctx, s, (int)strlen(s));     // raw: no newline translation needed
}

void fsterm_enter(shell_t *sh, fsterm_t *t) {
    memset(t, 0, sizeof(*t));
    put(sh, "\x1b[?1049h\x1b[?7l");
    t->want_size = true;
}

void fsterm_ask_size(fsterm_t *t) {
    t->want_size = true;
}

bool fsterm_size_key(shell_t *sh, fsterm_t *t, int key, int *rows, int *cols) {
    if (key != KEY_CURSOR_POS)
        return false;
    *rows = *cols = 0;
    if (t->size_asked) {
        t->size_asked = false;
        t->size_wait_until = 0;
        *rows = sh->keys.param[0];
        *cols = sh->keys.param[1];
    }
    return true;
}

bool fsterm_step(shell_t *sh, fsterm_t *t, int *log_waiting) {
    uint64_t now = time_us_64();
    if (t->size_asked && now >= t->size_expires)
        t->size_asked = false;
    // Only the console's log is held while a full-screen job runs there (main.c's log hold).
    if (sh->io->admin && now >= t->next_log_check) {
        *log_waiting = log_pending();
        t->next_log_check = now + LOG_CHECK_US;
    }
    if (t->want_size && shell_write_space(sh) >= 32) {
        put(sh, "\x1b[999;999H\x1b[6n");
        t->want_size = false;
        t->size_asked = true;
        t->size_wait_until = now + SIZE_WAIT_US;
        t->size_expires = now + SIZE_ANSWER_US;
    }
    return now >= t->size_wait_until;
}

void fsterm_send(shell_t *sh, fsterm_t *t, screen_t *scr) {
    int space;
    while (!screen_drawn(scr) && (space = shell_write_space(sh)) > 0) {
        size_t n = screen_draw(scr, t->out, (size_t)space < sizeof(t->out) ? (size_t)space
                                                                           : sizeof(t->out));
        if (!n)
            break;                          // too little room to make progress: next pass
        sh->io->write(sh->ctx, t->out, (int)n);
    }
}

void fsterm_leave(shell_t *sh, const screen_t *scr, bool cancel) {
    if (cancel && !sh->io->admin)
        return;
    if (scr->row_sent < scr->row_len)
        sh->io->write(sh->ctx, scr->row + scr->row_sent, (int)(scr->row_len - scr->row_sent));
    char bye[48];
    snprintf(bye, sizeof(bye), "\x1b[%d;1H\x1b[K\x1b[?7h\x1b[?1049l", scr->rows);
    put(sh, bye);
}
