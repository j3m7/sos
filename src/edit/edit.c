// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// `edit <file>`: connects the editor (editor.c) to a shell and the card. The file is read whole
// into RAM, edited there, and written back on Ctrl-S through a temporary file, so an interrupted
// save never leaves half a file under the real name.
//
// The editor runs as a raw-mode shell job: keys arrive through edit_key(), and each main-loop pass
// edit_step() sends as much of the screen update as the channel takes without waiting. It uses
// the terminal's alternate screen (xterm, and most terminals since), so the shell's output is
// back as it was afterwards, with autowrap off (a wide character then can't push a row onto the
// next one: it only hides the end of its own row), and asks the terminal for its size by moving the cursor to the
// far corner and asking where it ended up.

#include "edit/edit.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "ff.h"

#include "core/heap.h"
#include "core/heaptag.h"
#include "core/log.h"
#include "edit/editor.h"
#include "files/files.h"
#include "files/path.h"
#include "files/sdcard.h"

#define PATH_LEN        256
#define TMP_SUFFIX      ".~edit"
// Memory: the editor's buffer is allocated whole when the file is opened (editor_open()), for
// the file plus GROWTH of typing room, so a small file costs little while an editor sits open
// for an hour. Less when the heap has less to spare: HEAP_RESERVE is left free (a TLS handshake
// needs about this much, see the README), so an editor can't take what SSH and HTTPS need to
// keep working; and if a block that size can't be had (the heap is fragmented), the room is
// halved down to MIN_ROOM before giving up. Save and open again for more room.
#define HEAP_RESERVE    (64 * 1024)
#define GROWTH          (32 * 1024)
#define MIN_ROOM        1024
#define SIZE_WAIT_US    500000      // first frame: wait this long for the terminal's size
#define SIZE_ANSWER_US  3000000     // a cursor position report later than this isn't the answer
#define LOG_CHECK_US    1000000     // how often to count held log messages (console)

typedef struct {
    editor_t ed;
    char path[PATH_LEN];
    char out[2048];                 // screen output and save buffer (SSH takes 2048 a pass)
    FIL file;
    bool quit;
    bool want_size;                 // send the size query when there's room
    bool size_asked;                // ... sent: the next cursor position report is the answer
    uint64_t size_wait_until;       // no drawing before then (unless the answer comes)
    uint64_t size_expires;          // size_asked ends then (Ctrl+F3 sends the same bytes)
    uint64_t next_log_check;
} edit_t;

static void put(shell_t *sh, const char *s) {
    sh->io->write(sh->ctx, s, (int)strlen(s));     // raw: no newline translation needed
}

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
        while (fr == FR_OK && (n = editor_export(&e->ed, &pos, e->out, sizeof(e->out))) > 0) {
            UINT wrote;
            fr = f_write(&e->file, e->out, (UINT)n, &wrote);
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
    if (key == KEY_CURSOR_POS) {
        if (e->size_asked) {
            e->size_asked = false;
            e->size_wait_until = 0;
            editor_resize(&e->ed, sh->keys.param[0], sh->keys.param[1]);
        }
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
        e->want_size = true;
        break;
    case EDITOR_NONE:
        break;
    }
}

static bool edit_step(shell_t *sh, void *arg, bool cancel) {
    edit_t *e = arg;
    if (cancel || e->quit) {
        // A cancel means the session is gone, except on the console, which cancels the editor
        // when it locks itself (shell/login.h) with the terminal possibly still there: give
        // that terminal its screen back too, for the password prompt.
        if (!cancel || sh->io->admin) {
            // A row cut short between passes goes out to its end first, so the terminal never
            // sees a sequence or character cut by what follows.
            editor_t *ed = &e->ed;
            if (ed->row_sent < ed->row_len)
                sh->io->write(sh->ctx, ed->row + ed->row_sent, (int)(ed->row_len - ed->row_sent));
            // Bottom row, erased, for a terminal without an alternate screen (the prompt goes
            // there); autowrap back on; back to the shell's screen.
            char bye[48];
            snprintf(bye, sizeof(bye), "\x1b[%d;1H\x1b[K\x1b[?7h\x1b[?1049l", e->ed.rows);
            put(sh, bye);
        }
        editor_free(&e->ed);
        heap_release(HEAP_EDIT, e);
        return false;
    }
    uint64_t now = time_us_64();
    if (e->size_asked && now >= e->size_expires)
        e->size_asked = false;
    // Only the console's log is held while the editor runs there (main.c's log hold).
    if (sh->io->admin && now >= e->next_log_check) {
        editor_set_log_waiting(&e->ed, log_pending());
        e->next_log_check = now + LOG_CHECK_US;
    }
    if (e->want_size && shell_write_space(sh) >= 32) {
        put(sh, "\x1b[999;999H\x1b[6n");
        e->want_size = false;
        e->size_asked = true;
        e->size_wait_until = now + SIZE_WAIT_US;
        e->size_expires = now + SIZE_ANSWER_US;
    }
    if (now < e->size_wait_until)
        return true;
    int space;
    while (!editor_drawn(&e->ed) && (space = shell_write_space(sh)) > 0) {
        size_t n = editor_draw(&e->ed, e->out, (size_t)space < sizeof(e->out) ? (size_t)space
                                                                              : sizeof(e->out));
        if (!n)
            break;                          // the next row needs more room: next pass
        sh->io->write(sh->ctx, e->out, (int)n);
    }
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
    put(sh, "\x1b[?1049h\x1b[?7l");         // the alternate screen, autowrap off
    e->want_size = true;
    shell_run_raw(sh, edit_step, edit_key, e);
}
