// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// `more <file>` and edit_view(): the pager (pager.c) on a shell, over a file on the card (read
// through a window, so the file can be larger than RAM) or over a text in RAM (a copy, for the
// manual pages). A raw-mode shell job, like the editor; the terminal side is fsterm.c.
//
// `<command> | more` with a command that still runs (edit_view_growing()): the text is the
// shell's capture, and each pass steps the command once through shell_pipe_feed() before
// drawing, so its output grows the text while the pager shows it.

#include "edit/edit.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"

#include "core/heaptag.h"
#include "edit/fsterm.h"
#include "edit/pager.h"
#include "files/files.h"
#include "files/path.h"
#include "files/sdcard.h"

#define PATH_LEN 256

typedef struct {
    pager_t pg;
    fsterm_t term;
    FIL file;
    bool file_open;                 // reading the card: files_hold() until it's closed
    char *text;                     // or a copy of a text in RAM (ours to free)
    bool quit;
    bool live;                      // the command under the pager still runs: the text is the
                                    // shell's capture until shell_pipe_feed() hands it over
    bool stop;                      // Ctrl-C: end that command, keep the text
} view_t;

static size_t read_file(void *ctx, size_t off, char *buf, size_t n) {
    view_t *v = ctx;
    UINT got = 0;
    if (f_lseek(&v->file, off) != FR_OK || f_read(&v->file, buf, (UINT)n, &got) != FR_OK)
        return 0;
    return got;
}

static void view_key(shell_t *sh, int key, void *arg) {
    view_t *v = arg;
    int rows, cols;
    if (v->quit)
        return;
    if (fsterm_size_key(sh, &v->term, key, &rows, &cols)) {
        if (rows)
            pager_resize(&v->pg, rows, cols);
        return;
    }
    if (v->live && key == 0x03 && !v->pg.finding) {  // Ctrl-C stops the command, not the pager
        v->stop = true;
        return;
    }
    switch (pager_key(&v->pg, key)) {
    case PAGER_QUIT:
        v->quit = true;
        break;
    case PAGER_RESIZE:
        fsterm_ask_size(&v->term);
        break;
    case PAGER_NONE:
        break;
    }
}

// The free part of leaving.
static void view_free(view_t *v) {
    if (v->file_open) {
        f_close(&v->file);
        files_release();
    }
    pager_free(&v->pg);
    heap_release(HEAP_EDIT, v->text);
    heap_release(HEAP_EDIT, v);
}

// The command under the pager, once a pass: a step (or its end), then the pager follows the text,
// which may have moved (the shell's block is reallocated as it grows). False when the pager must
// leave at once (the command asked a question: the shell shows the text and asks it).
static bool view_feed(shell_t *sh, view_t *v) {
    char *text;
    size_t len;
    bool stop = v->stop;
    shell_fed_t r = shell_pipe_feed(sh, stop ? SHELL_FEED_STOP : SHELL_FEED_STEP, &text, &len);
    if (r == SHELL_FED_GONE) {
        v->live = false;
        return false;
    }
    v->pg.src.ctx = text;           // first: pager_grow() reads the text's last byte
    if (len > v->pg.src.size)
        pager_grow(&v->pg, len);
    if (r == SHELL_FED_DONE) {
        v->live = false;
        v->text = text;             // ours from here on
        pager_set_growing(&v->pg, false);
        if (stop)
            pager_message(&v->pg, "stopped");
    }
    return true;
}

static bool view_step(shell_t *sh, void *arg, bool cancel) {
    view_t *v = arg;
    if (v->live && (cancel || v->quit)) {   // the command goes first, silently, with its text
        char *text;
        size_t len;
        shell_pipe_feed(sh, SHELL_FEED_CANCEL, &text, &len);
        v->live = false;
    }
    if (!cancel && !v->quit && v->live && !view_feed(sh, v))
        v->quit = true;
    if (cancel || v->quit) {
        fsterm_leave(sh, &v->pg.scr, cancel);
        view_free(v);
        return false;
    }
    int waiting = v->pg.log_waiting;
    bool draw = fsterm_step(sh, &v->term, &waiting);
    pager_set_log_waiting(&v->pg, waiting);
    if (draw)
        fsterm_send(sh, &v->term, &v->pg.scr);
    return true;
}

// Start the job on a source that is ready. Frees `v` (and its text, not the shell's) and says so
// if the pager can't start.
static bool run(shell_t *sh, view_t *v, const char *name, pager_source_t src) {
    if (!pager_init(&v->pg, name, src, 0)) {
        shell_printf(sh, "%s: out of memory\n", name);
        view_free(v);
        return false;
    }
    fsterm_enter(sh, &v->term);
    shell_run_raw(sh, view_step, view_key, v);
    return true;
}

void edit_view_file(shell_t *sh, const char *path, const char *name) {
    // `name` heads the error messages and the status row (the path itself for `more`).
    view_t *v = heap_zalloc(HEAP_EDIT, sizeof(*v));
    if (!v) {
        shell_printf(sh, "%s: out of memory\n", name);
        return;
    }
    if (!sdcard_mount(sh)) {
        heap_release(HEAP_EDIT, v);
        return;
    }
    FILINFO fi;
    FRESULT fr = f_stat(path, &fi);
    if (fr == FR_OK && (fi.fattrib & AM_DIR)) {
        shell_printf(sh, "%s: %s: is a directory\n", name, path);
        heap_release(HEAP_EDIT, v);
        return;
    }
    if (fr == FR_OK)
        fr = f_open(&v->file, path, FA_READ);
    if (fr != FR_OK) {
        shell_printf(sh, "%s: %s: %s\n", name, path, files_error(fr));
        heap_release(HEAP_EDIT, v);
        return;
    }
    v->file_open = true;
    files_hold();
    pager_source_t src = { .read = read_file, .ctx = v, .size = (size_t)f_size(&v->file) };
    run(sh, v, !strcmp(name, "more") ? path : name, src);
}

void view_command(shell_t *sh, int argc, char **argv) {
    if (argc != 2 || !*argv[1]) {           // the file is one word (quoted, it may hold spaces)
        shell_printf(sh, "usage: more <file>\n");
        return;
    }
    const char *file = argv[1];
    char path[PATH_LEN];
    if (!path_resolve(sh->cwd, file, path, sizeof(path))) {
        shell_printf(sh, "path too long\n");
        return;
    }
    if (shell_output_piped(sh)) {           // `more <file> | tail 5`: the text, not the pager
        files_cat(sh, "more", file);
        return;
    }
    edit_view_file(sh, path, "more");
}

void edit_view_take(shell_t *sh, const char *name, char *text, size_t len) {
    view_t *v = heap_zalloc(HEAP_EDIT, sizeof(*v));
    if (!v) {
        shell_printf(sh, "%s: out of memory\n", name);
        heap_release(HEAP_EDIT, text);
        return;
    }
    v->text = text;
    pager_source_t src = { .read = pager_read_mem, .ctx = text, .size = len };
    run(sh, v, name, src);
}

bool edit_view_growing(shell_t *sh, const char *name, char *text, size_t len) {
    view_t *v = heap_zalloc(HEAP_EDIT, sizeof(*v));
    if (!v) {
        shell_printf(sh, "%s: out of memory\n", name);
        return false;
    }
    v->live = true;
    // Growing: the window is allocated whole now, as the size says nothing about the end.
    pager_source_t src = { .read = pager_read_mem, .ctx = text, .size = len,
                           .growing = true };
    return run(sh, v, name, src);
}

void edit_view(shell_t *sh, const char *name, const char *text, size_t len) {
    char *copy = heap_alloc(HEAP_EDIT, len ? len : 1);
    if (!copy) {
        shell_printf(sh, "%s: out of memory\n", name);
        return;
    }
    memcpy(copy, text, len);
    edit_view_take(sh, name, copy, len);
}
