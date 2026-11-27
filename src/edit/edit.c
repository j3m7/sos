// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// `edit <file>`: connects the editor (editor.c) to a shell and the card. The file is read whole
// into RAM, edited there, and written back on Ctrl-S through a temporary file, so an interrupted
// save never leaves half a file under the real name.
//
// The editor runs as a raw-mode shell job: keys arrive through edit_key(), and each main-loop pass
// edit_step() sends as much of the screen update as the channel takes without waiting. The
// terminal side (the alternate screen, the size query, held log messages) is fsterm.c.

#include "edit/edit.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "ff.h"

#include "core/heap.h"
#include "core/heaptag.h"
#include "edit/editor.h"
#include "edit/fsterm.h"
#include "files/files.h"
#include "files/path.h"
#include "files/sdcard.h"

#define PATH_LEN        256
#define TMP_SUFFIX      ".~edit"
// Memory: the editor's buffer is allocated whole when the file is opened (editor_open()), for
// the file plus GROWTH of typing room, so a small file costs little while an editor sits open
// for an hour. Less when the heap has less to spare: HEAP_RESERVE is left free (a TLS handshake
// needs about this much, see docs/tls.md), so an editor can't take what SSH and HTTPS need to
// keep working; and if a block that size can't be had (the heap is fragmented), the room is
// halved down to MIN_ROOM before giving up. Save and open again for more room.
#define HEAP_RESERVE    (64 * 1024)
#define GROWTH          (32 * 1024)
#define MIN_ROOM        1024

typedef struct {
    editor_t ed;
    fsterm_t term;                  // its out[] is the save buffer too
    char path[PATH_LEN];
    FIL file;
    bool quit;
} edit_t;

// A size for a message: whole KB once rounding down loses under a tenth, else bytes.
static const char *size_text(char *buf, size_t cap, size_t n) {
    if (n >= 10 * 1024)
        snprintf(buf, cap, "%lu KB", (unsigned long)(n / 1024));
    else
        snprintf(buf, cap, "%lu bytes", (unsigned long)n);
    return buf;
}

// --- saving --------------------------------------------------------------------------------------

static void save(shell_t *sh, edit_t *e) {
    char tmp[PATH_LEN + sizeof(TMP_SUFFIX)];
    char msg[96];
    snprintf(tmp, sizeof(tmp), "%s%s", e->path, TMP_SUFFIX);
    if (!files_write_allowed(sh, e->path) || !files_write_allowed(sh, tmp)) {
        editor_saved(&e->ed, false, "not saved: this path can only be changed on the console");
        return;
    }
    if (!sdcard_mount(NULL)) {
        editor_saved(&e->ed, false, "not saved: no card");
        return;
    }
    files_hold();
    FRESULT fr = f_open(&e->file, tmp, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr == FR_OK) {
        size_t pos = 0, n;
        while (fr == FR_OK && (n = editor_export(&e->ed, &pos, e->term.out, sizeof(e->term.out))) > 0) {
            UINT wrote;
            fr = f_write(&e->file, e->term.out, (UINT)n, &wrote);
            if (fr == FR_OK && wrote != n)
                fr = FR_DENIED;             // card full
        }
        FRESULT cr = f_close(&e->file);
        if (fr == FR_OK)
            fr = cr;
        if (fr != FR_OK) {
            f_unlink(tmp);
        } else {
            // FatFs can't rename over a file: remove the old one first. If the rename then
            // fails, the text is safe under the temporary name.
            FRESULT ur = f_unlink(e->path);
            if (ur != FR_OK && ur != FR_NO_FILE) {
                f_unlink(tmp);
                fr = ur;
            } else if ((fr = f_rename(tmp, e->path)) != FR_OK) {
                // Still "modified": quitting warns, and Ctrl-S tries again.
                files_release();
                snprintf(msg, sizeof(msg), "only saved as %s: %s", path_basename(tmp),
                         files_error(fr));
                editor_saved(&e->ed, false, msg);
                return;
            }
        }
    }
    files_release();
    if (fr == FR_OK)
        snprintf(msg, sizeof(msg), "saved, %lu bytes", (unsigned long)editor_saved_len(&e->ed));
    else
        snprintf(msg, sizeof(msg), "not saved: %s", files_error(fr));
    editor_saved(&e->ed, fr == FR_OK, msg);
}

// --- the job -------------------------------------------------------------------------------------

static void edit_key(shell_t *sh, int key, void *arg) {
    edit_t *e = arg;
    if (e->quit)
        return;
    int rows, cols;
    if (fsterm_size_key(sh, &e->term, key, &rows, &cols)) {
        if (rows)
            editor_resize(&e->ed, rows, cols);
        return;
    }
    switch (editor_key(&e->ed, key)) {
    case EDITOR_SAVE:
        save(sh, e);
        break;
    case EDITOR_QUIT:
        e->quit = true;
        break;
    case EDITOR_RESIZE:
        fsterm_ask_size(&e->term);
        break;
    case EDITOR_NONE:
        break;
    }
}

static bool edit_step(shell_t *sh, void *arg, bool cancel) {
    edit_t *e = arg;
    if (cancel || e->quit) {
        fsterm_leave(sh, &e->ed.scr, cancel);
        editor_free(&e->ed);
        heap_release(HEAP_EDIT, e);
        return false;
    }
    int waiting = e->ed.log_waiting;
    bool draw = fsterm_step(sh, &e->term, &waiting);
    editor_set_log_waiting(&e->ed, waiting);
    if (draw)
        fsterm_send(sh, &e->term, &e->ed.scr);
    return true;
}

// --- the command ---------------------------------------------------------------------------------

// The most text the editor may hold now: what the heap can spare, up to EDITOR_MAX_TEXT.
static size_t text_limit(void) {
    size_t free = heap_free();
    size_t limit = free > HEAP_RESERVE ? free - HEAP_RESERVE : 0;
    return limit > EDITOR_MAX_TEXT ? EDITOR_MAX_TEXT : limit;
}

// Whether the file is there and can be edited, and its size. False after saying why; true with
// *exists false for a file that isn't there (if that's allowed).
static bool stat_file(shell_t *sh, const char *path, bool quiet_missing, bool *exists, size_t *size) {
    FILINFO fi;
    FRESULT fr = f_stat(path, &fi);
    *exists = fr == FR_OK;
    *size = 0;
    if (fr == FR_NO_FILE && quiet_missing)
        return true;                        // a new file
    if (fr != FR_OK) {
        shell_printf(sh, "edit: %s: %s\n", path, files_error(fr));
        return false;
    }
    if (fi.fattrib & AM_DIR) {
        shell_printf(sh, "edit: %s: is a directory\n", path);
        return false;
    }
    *size = (size_t)fi.fsize;
    return true;
}

// Open the editor on `size` bytes read from `path` (0 bytes and no path: a new file), straight
// into the editor's buffer. The status bar shows e->path whichever file the text came from, and
// so do the messages here. False after saying why; the editor is then free.
static bool read_file(shell_t *sh, edit_t *e, const char *path, size_t size) {
    size_t most = text_limit();
    char a[24], b[24];
    if (size > EDITOR_MAX_TEXT) {
        shell_printf(sh, "edit: %s: too big to edit (%s, %u KB at most)\n", e->path,
                     size_text(a, sizeof(a), size), EDITOR_MAX_TEXT / 1024);
        return false;
    }
    if (most < size + MIN_ROOM) {
        shell_printf(sh, "edit: %s: not enough free memory to edit it (%s, %s free for the "
                     "editor)\n", e->path, size_text(a, sizeof(a), size), size_text(b, sizeof(b), most));
        return false;
    }
    size_t room = GROWTH;
    if (most - size < room)
        room = most - size;
    char *fill;
    while (!(fill = editor_open(&e->ed, e->path, size, size + room)) && room / 2 >= MIN_ROOM)
        room /= 2;                          // fragmented: a smaller block may still be had
    if (!fill) {
        shell_printf(sh, "edit: %s: out of memory: no block of %s free (%s in all); closing an SSH "
                     "session may help\n", e->path, size_text(a, sizeof(a), size + room),
                     size_text(b, sizeof(b), most));
        return false;
    }
    if (path) {
        UINT got = 0;
        FRESULT fr = f_open(&e->file, path, FA_READ);
        if (fr == FR_OK && f_size(&e->file) != size) {      // grown since stat_file() (SFTP, HTTP)
            f_close(&e->file);
            shell_printf(sh, "edit: %s: changed while opening, try again\n", path);
            editor_free(&e->ed);
            return false;
        }
        if (fr == FR_OK) {
            fr = f_read(&e->file, fill, (UINT)size, &got);
            f_close(&e->file);
        }
        if (fr != FR_OK || got != size) {
            shell_printf(sh, "edit: %s: %s\n", path, fr != FR_OK ? files_error(fr) : "short read");
            editor_free(&e->ed);
            return false;
        }
    }
    editor_opened(&e->ed);
    return true;
}

void edit_command(shell_t *sh, char *args) {
    if (!args || !*args || strchr(args, ' ')) {
        shell_printf(sh, "usage: edit <file>\n");
        return;
    }
    edit_t *e = heap_zalloc(HEAP_EDIT, sizeof(*e));
    if (!e) {
        shell_printf(sh, "edit: out of memory\n");
        return;
    }
    if (!path_resolve(sh->cwd, args, e->path, sizeof(e->path))) {
        shell_printf(sh, "path too long\n");
        heap_release(HEAP_EDIT, e);
        return;
    }
    if (strlen(e->path) + sizeof(TMP_SUFFIX) > PATH_LEN) {
        shell_printf(sh, "edit: path too long\n");
        heap_release(HEAP_EDIT, e);
        return;
    }
    bool exists;
    size_t size;
    if (!sdcard_mount(sh) || !stat_file(sh, e->path, true, &exists, &size)) {
        heap_release(HEAP_EDIT, e);
        return;
    }
    // A save interrupted (power lost) between removing the old file and renaming the new one
    // leaves the text only in <file>.~edit: open that instead of an empty file. If the first
    // save of a new file was interrupted while writing, it's partial: the message says so.
    char tmp[PATH_LEN + sizeof(TMP_SUFFIX)];
    snprintf(tmp, sizeof(tmp), "%s%s", e->path, TMP_SUFFIX);
    FILINFO tfi;
    bool tmp_left = f_stat(tmp, &tfi) == FR_OK && !(tfi.fattrib & AM_DIR);
    bool recovered = tmp_left && !exists;
    bool ok;
    if (recovered) {
        bool tmp_exists;
        ok = stat_file(sh, tmp, false, &tmp_exists, &size) && read_file(sh, e, tmp, size);
    } else {
        ok = read_file(sh, e, exists ? e->path : NULL, size);
    }
    if (!ok) {
        heap_release(HEAP_EDIT, e);
        return;
    }
    bool is_new = !exists && !recovered;
    if (recovered)
        e->ed.modified = true;              // saving puts it under the real name
    if (!files_write_allowed(sh, e->path))
        editor_message(&e->ed, "read only: this path can only be changed on the console");
    else if (recovered)
        editor_message(&e->ed, "recovered from an interrupted save (" TMP_SUFFIX
                       "), may be incomplete: Ctrl-S saves it");
    else if (tmp_left)
        editor_message(&e->ed, "an interrupted save left " TMP_SUFFIX " (incomplete): saving replaces it");
    else if (is_new)
        editor_message(&e->ed, "new file; Ctrl-S save, Ctrl-Q quit, Ctrl-F find, Ctrl-L redraw");
    fsterm_enter(sh, &e->term);
    shell_run_raw(sh, edit_step, edit_key, e);
}
