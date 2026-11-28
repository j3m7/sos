// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// tar cf and tar czf (docs/design/archives.md §6.7): files and directories on the card into a tar
// archive, plain or gzipped (files/gzw.h), with headers from files/tar_write.h, as a shell job.
//
// Each step does a bounded amount of work for STEP_US: a directory entry read and its header
// written, or a 4 KB read from a file and its write (through deflate for `cz`). Directories are
// walked in directory order. The walk's stack is in the job, on the heap, not on the C stack: per
// level a FatFs DIR, how many entries have been read from it, and the length of its path and of
// its stored name (prefixes of the current ones), at most LEVELS deep. FatFs counts an open
// subdirectory against its lock table (FF_FS_LOCK, 16 objects for everyone: the log writer, SFTP
// with up to 8, other shells), so at most DIRS_OPEN levels are open at a time: going deeper closes
// the outermost open one, and coming back to a closed level opens it again and reads past the
// entries already taken. A directory changed meanwhile may then have an entry missed or taken
// twice; GNU tar has the same race.
//
// Skipped with a note, the archive still made: the archive itself and its .part, a file being
// written (no date in its directory entry yet, as `ls` shows it, or locked by FatFs because it is
// open for writing), anything that can't be opened or read, names too long for the reader
// (TAR_NAME_MAX), directories deeper than LEVELS, paths given that don't exist. A file that
// shrinks while it is read is padded with zeros to the size its header gave. The archive is
// written to <archive>.part and put in place with files_install_part() at the end, replacing an
// existing one only with -f. Ctrl-C, a write error or a full card deletes the .part; so does an
// archive with nothing in it.
//
// Dates: FAT dates on the card are UTC (the SD library's get_fattime() runs localtime_r() with
// no zone set, and SOS keeps zones display-only), so a file's date goes into its header as it is.
//
// Memory: the job (two FatFs files, the walk, paths, two 4 KB buffers: ~14 KB) and, for `cz`,
// zlib's ~54 KB (GZW_HEAP). Refused when that would leave less than HEAP_KEEP free; and one at a
// time across the shells (files/bigjob.h).

#include "files/tar.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "pico/stdlib.h"

#include "ff.h"

#include "core/heap.h"
#include "core/heaptag.h"
#include "files/bigjob.h"
#include "files/files.h"
#include "files/gzw.h"
#include "files/path.h"
#include "files/sdcard.h"
#include "files/tar_parse.h"
#include "files/tar_write.h"
#include "time/timeconv.h"

#define PATH_LEN    256             // FatFs long names are up to 255 characters
#define ABS_LEN     (PATH_LEN + TAR_NAME_MAX)   // -C's directory, '/', and a stored name
#define STEP_US     20000           // work per job step before letting the main loop run
#define BUF         4096            // a file's data read, and deflate's output
#define HEAP_KEEP   (64u * 1024u)   // heap left free while a job runs (a TLS handshake)
#define LEVELS      16              // directories deep the walk goes
#define DIRS_OPEN   4               // and how many of them it keeps open at a time
#define LINE_MAX    (TAR_NAME_MAX + 96)
#define PART        ".part"

typedef struct {
    DIR dir;
    bool open;                      // `dir` is open (else it is opened again when needed)
    uint32_t read;                  // entries taken from it so far
    uint32_t skip;                  // opened again: entries still to read past
    size_t abs_len, rel_len;        // the directory's path and stored name: their lengths
} level_t;

typedef struct {
    bool gz, force, verbose;
    bool said_strip;                // "removing leading /" said once
    bool in_open;                   // a file's data is being copied
    bool shrank;                    // and it ended early: zeros make up the rest
    char **names;                   // the archive and the paths as typed (copies, one block)
    char **paths;                   // names + 1
    int n, next;
    int depth;                      // levels in the walk
    int open;                       // and how many of them are open
    const char *stop;               // why the job stops (static text), or NULL
    FIL in, out;
    const char *arg;                // the archive as typed (a copy, in `paths`' block)
    char arc[PATH_LEN];
    char part[PATH_LEN + sizeof(PART)];
    char target[PATH_LEN];          // -C, or the working directory
    char abs[ABS_LEN];              // the entry's path on the card
    char rel[TAR_NAME_MAX];         // and its name in the archive
    uint64_t size, left;            // in_open: the header's size, and how much is still to write
    uint64_t written;               // archive bytes made (before gzip)
    uint64_t out_bytes;             // bytes written to the .part
    uint64_t synced_at;
    unsigned files, dirs, skipped;
    level_t level[LEVELS];
    gzw_t gzw;
    char line[LINE_MAX];
    uint8_t buf[BUF];
    uint8_t zbuf[BUF];
} tarc_t;

static const uint8_t zero_block[TAR_BLOCK];

// --- output ------------------------------------------------------------------------------------

// "tar: <name>: <why>\n", or with `why` NULL just "<name>\n" (v): the name in full
// (shell_printf() cuts at 255 bytes), control characters shown as '?'.
static void say(shell_t *sh, tarc_t *j, const char *name, const char *why) {
    size_t len = why ? (size_t)snprintf(j->line, sizeof(j->line), "tar: ") : 0;
    for (; *name && len < sizeof(j->line) - 64; name++) {
        unsigned char c = (unsigned char)*name;
        j->line[len++] = c < 0x20 || c == 0x7f ? '?' : (char)c;
    }
    int w = snprintf(j->line + len, sizeof(j->line) - len, "%s%s\n", why ? ": " : "",
                     why ? why : "");
    if (w > 0)
        len += (size_t)w < sizeof(j->line) - len ? (size_t)w : sizeof(j->line) - len - 1;
    shell_write(sh, j->line, len);
}

static void skip(shell_t *sh, tarc_t *j, const char *name, const char *why) {
    say(sh, j, name, why);
    j->skipped++;
}

// Bytes for the .part. False (j->stop set) on a write error or a full card.
static bool put(tarc_t *j, const uint8_t *p, size_t n) {
    UINT bw;
    FRESULT fr = f_write(&j->out, p, (UINT)n, &bw);
    if (fr == FR_OK && bw != n) {
        j->stop = "card full";
        return false;
    }
    if (fr == FR_OK)
        fr = files_sync_if_due(&j->out, &j->synced_at);
    if (fr != FR_OK) {
        j->stop = files_error((int)fr);
        return false;
    }
    j->out_bytes += n;
    return true;
}

// Archive bytes: to the .part, or through deflate (`last`: the archive's end). False (j->stop
// set) on an error.
static bool emit(tarc_t *j, const uint8_t *p, size_t n, bool last) {
    j->written += n;
    if (!j->gz)
        return put(j, p, n);
    for (;;) {
        size_t made;
        if (!gzw_feed(&j->gzw, &p, &n, last, j->zbuf, sizeof(j->zbuf), &made)) {
            j->stop = j->gzw.error;
            return false;
        }
        if (made && !put(j, j->zbuf, made))
            return false;
        if (!n && made < sizeof(j->zbuf) && (!last || gzw_done(&j->gzw)))
            return true;
    }
}

static bool emit_zeros(tarc_t *j, size_t n) {
    for (; n > TAR_BLOCK; n -= TAR_BLOCK)
        if (!emit(j, zero_block, TAR_BLOCK, false))
            return false;
    return !n || emit(j, zero_block, n, false);
}

// --- entries -----------------------------------------------------------------------------------

// A FAT date (UTC on this card) as seconds since 1970; 0 for none.
static int64_t fat_time(const FILINFO *fi) {
    if (!fi->fdate)
        return 0;
    int64_t days = timeconv_days_from_civil((fi->fdate >> 9) + 1980, (fi->fdate >> 5) & 15,
                                            fi->fdate & 31);
    return days * 86400 + (fi->ftime >> 11) * 3600 + ((fi->ftime >> 5) & 63) * 60 +
           (fi->ftime & 31) * 2;
}

// The header for j->rel. False (j->stop set) on an error.
static bool header(tarc_t *j, bool dir, uint64_t size, const FILINFO *fi) {
    size_t len = tarw_header(j->buf, j->rel, dir, size, fat_time(fi));
    return emit(j, j->buf, len, false);     // len > 0: j->rel was checked
}

// The directory j->abs: its header (unless it is -C's directory itself: j->rel empty), then into
// the walk. False if the job must stop.
static bool add_dir(shell_t *sh, tarc_t *j, const FILINFO *fi) {
    if (j->depth == LEVELS) {
        char why[48];
        snprintf(why, sizeof(why), "more than %d directories deep, skipped", LEVELS);
        skip(sh, j, j->rel, why);
        return true;
    }
    if (j->open == DIRS_OPEN) {             // the outermost open level makes room
        level_t *o = j->level;
        while (!o->open)
            o++;
        f_closedir(&o->dir);
        o->open = false;
        j->open--;
    }
    level_t *l = &j->level[j->depth];
    FRESULT fr = f_opendir(&l->dir, j->abs);
    if (fr != FR_OK) {
        skip(sh, j, j->rel[0] ? j->rel : j->abs, files_error((int)fr));
        return true;
    }
    l->open = true;
    l->read = l->skip = 0;
    j->open++;
    if (j->rel[0]) {
        if (!header(j, true, 0, fi)) {
            f_closedir(&l->dir);
            l->open = false;
            j->open--;
            return false;
        }
        j->dirs++;
        if (j->verbose) {
            size_t len = strlen(j->rel);
            j->rel[len] = '/';
            j->rel[len + 1] = '\0';
            say(sh, j, j->rel, NULL);
            j->rel[len] = '\0';
        }
    }
    l->abs_len = strlen(j->abs);
    l->rel_len = strlen(j->rel);
    j->depth++;
    return true;
}

// The file j->abs: checked, opened, its header; its data follows in later pieces. False if the
// job must stop.
static bool add_file(shell_t *sh, tarc_t *j, const FILINFO *fi) {
    if (!strcasecmp(j->abs, j->arc) || !strcasecmp(j->abs, j->part)) {
        skip(sh, j, j->rel, "the archive itself, skipped");
        return true;
    }
    if (files_being_written(fi)) {
        skip(sh, j, j->rel, "being written, skipped");
        return true;
    }
    FRESULT fr = f_open(&j->in, j->abs, FA_READ);
    if (fr != FR_OK) {
        skip(sh, j, j->rel, fr == FR_LOCKED ? "being written, skipped" : files_error((int)fr));
        return true;
    }
    uint64_t size = f_size(&j->in);
    if (!header(j, false, size, fi)) {
        f_close(&j->in);
        return false;
    }
    j->files++;
    if (j->verbose)
        say(sh, j, j->rel, NULL);
    j->in_open = true;
    j->shrank = false;
    j->size = j->left = size;
    return true;
}

// A piece of the open file's data, or its end: the padding to the next block.
static bool copy_data(shell_t *sh, tarc_t *j) {
    if (!j->left) {
        j->in_open = false;
        f_close(&j->in);
        return emit_zeros(j, tarw_pad(j->size));
    }
    size_t n = j->left < BUF ? (size_t)j->left : BUF;
    UINT br = 0;
    if (!j->shrank) {
        FRESULT fr = f_read(&j->in, j->buf, (UINT)n, &br);
        if (fr != FR_OK || br == 0) {
            say(sh, j, j->rel, fr != FR_OK ? files_error((int)fr) : "shrank while read");
            say(sh, j, j->rel, "padded with zeros to its size");
            j->shrank = true;
        }
    }
    if (j->shrank) {
        memset(j->buf, 0, n);
        br = (UINT)n;
    }
    j->left -= br;
    return emit(j, j->buf, br, false);
}

// The next path given: its directory or file. False if the job must stop.
static bool start_path(shell_t *sh, tarc_t *j) {
    const char *arg = j->paths[j->next++];
    bool stripped = false;
    const char *why = tar_name_clean(arg, j->rel, TAR_NAME_MAX - 1, &stripped);
    if (stripped && !j->said_strip) {
        shell_printf(sh, "tar: removing leading / from names\n");
        j->said_strip = true;
    }
    if (why) {
        skip(sh, j, arg, why);
        return true;
    }
    if (!path_resolve(j->target, arg, j->abs, PATH_LEN)) {
        skip(sh, j, arg, "path too long");
        return true;
    }
    FILINFO fi;
    memset(&fi, 0, sizeof(fi));
    if (!strcmp(j->abs, "/")) {
        fi.fattrib = AM_DIR;                // the root has no directory entry to stat
    } else {
        FRESULT fr = f_stat(j->abs, &fi);
        if (fr != FR_OK) {
            skip(sh, j, arg, fr == FR_NO_FILE || fr == FR_NO_PATH ? "no such file or directory"
                                                                  : files_error((int)fr));
            return true;
        }
    }
    if (fi.fattrib & AM_DIR)
        return add_dir(sh, j, &fi);
    return add_file(sh, j, &fi);
}

// The innermost level done (or failed): closed, off the walk.
static void pop(tarc_t *j) {
    level_t *l = &j->level[--j->depth];
    if (l->open) {
        f_closedir(&l->dir);
        l->open = false;
        j->open--;
    }
}

// The next entry of the innermost directory, or its end. A level closed to make room is opened
// again and read past what was taken, an entry a piece.
static bool walk(shell_t *sh, tarc_t *j) {
    level_t *l = &j->level[j->depth - 1];
    j->abs[l->abs_len] = '\0';
    j->rel[l->rel_len] = '\0';
    const char *name = l->rel_len ? j->rel : j->abs;
    FRESULT fr;
    if (!l->open) {                         // the deeper levels are gone: there is room
        fr = f_opendir(&l->dir, j->abs);
        if (fr != FR_OK) {
            skip(sh, j, name, files_error((int)fr));
            j->depth--;
            return true;
        }
        l->open = true;
        l->skip = l->read;
        j->open++;
    }
    FILINFO fi;
    fr = f_readdir(&l->dir, &fi);
    if (fr != FR_OK || !fi.fname[0]) {
        if (fr != FR_OK)
            skip(sh, j, name, files_error((int)fr));
        pop(j);
        return true;
    }
    if (l->skip) {
        l->skip--;
        return true;
    }
    l->read++;
    size_t name_len = strlen(fi.fname);
    size_t abs_sep = l->abs_len > 1 ? 1 : 0, rel_sep = l->rel_len ? 1 : 0;
    // The stored name must fit what tar_parse.c reads back, with a directory's '/'.
    if (l->abs_len + abs_sep + name_len >= ABS_LEN ||
        l->rel_len + rel_sep + name_len > TAR_NAME_MAX - 2) {
        char *rel = j->rel + l->rel_len;
        snprintf(rel, TAR_NAME_MAX - l->rel_len, "%s%s", rel_sep ? "/" : "", fi.fname);
        skip(sh, j, j->rel, "name too long, skipped");
        return true;
    }
    if (abs_sep)
        j->abs[l->abs_len] = '/';
    memcpy(j->abs + l->abs_len + abs_sep, fi.fname, name_len + 1);
    if (rel_sep)
        j->rel[l->rel_len] = '/';
    memcpy(j->rel + l->rel_len + rel_sep, fi.fname, name_len + 1);
    if (fi.fattrib & AM_DIR)
        return add_dir(sh, j, &fi);
    return add_file(sh, j, &fi);
}

// --- the job -----------------------------------------------------------------------------------

static bool job_end(tarc_t *j) {
    while (j->depth > 0)
        pop(j);
    if (j->in_open)
        f_close(&j->in);
    if (j->gz)
        gzw_end(&j->gzw);
    bigjob_end();
    files_release();
    heap_release(HEAP_FILES, (void *)j->names);
    heap_release(HEAP_FILES, j);
    return false;
}

// Stopped (`why`): the .part deleted, what was done said.
static bool stopped(shell_t *sh, tarc_t *j, const char *why) {
    f_close(&j->out);
    f_unlink(j->part);
    shell_printf(sh, "stopped: %s; %s not made\n", why, j->arg);
    return job_end(j);
}

// All the paths are in: the archive's end, then the .part into place.
static bool finish(shell_t *sh, tarc_t *j) {
    if (!j->files && !j->dirs) {
        f_close(&j->out);
        f_unlink(j->part);
        shell_printf(sh, "tar: nothing to archive; %s not made\n", j->arg);
        return job_end(j);
    }
    uint64_t end = tarw_end(j->written);
    for (; end > TAR_BLOCK; end -= TAR_BLOCK)
        if (!emit(j, zero_block, TAR_BLOCK, false))
            return stopped(sh, j, j->stop);
    if (!emit(j, zero_block, (size_t)end, true))
        return stopped(sh, j, j->stop);
    FRESULT fr = f_close(&j->out);
    if (fr != FR_OK) {
        f_unlink(j->part);
        shell_printf(sh, "tar: %s: %s\n", j->arg, files_error((int)fr));
        return job_end(j);
    }
    const char *why;
    files_install_t r = files_install_part(sh, "tar", "archive", j->part, j->arc, j->force, &why);
    if (r != FILES_INSTALLED) {
        if (r == FILES_NOT_INSTALLED)
            f_unlink(j->part);
        shell_printf(sh, "tar: %s: %s\n", j->arg, why);
        return job_end(j);
    }
    shell_printf(sh, "%s: %u files, %u directories, %llu bytes (%u skipped)\n", j->arg, j->files,
                 j->dirs, (unsigned long long)j->out_bytes, j->skipped);
    return job_end(j);
}

// One piece of work. False when the job is over (and ended).
static bool advance(shell_t *sh, tarc_t *j) {
    bool ok;
    if (j->in_open)
        ok = copy_data(sh, j);
    else if (j->depth)
        ok = walk(sh, j);
    else if (j->next < j->n)
        ok = start_path(sh, j);
    else
        return finish(sh, j);
    return ok || stopped(sh, j, j->stop);
}

static bool create_step(shell_t *sh, void *arg, bool cancel) {
    tarc_t *j = arg;
    if (cancel)
        return stopped(sh, j, "Ctrl-C");
    uint64_t t0 = time_us_64();
    do {                                    // at least one piece of work a step
        if (!advance(sh, j))
            return false;
    } while (time_us_64() - t0 < STEP_US);
    return true;
}

// --- the command -------------------------------------------------------------------------------

// The archive's name and the paths, copied into one block (argv lives only as long as the
// command's line): names[0] the archive, names[1..n] the paths, names[n + 1] NULL.
static char **copy_names(const char *archive, int n, char **paths) {
    size_t bytes = (size_t)(n + 2) * sizeof(char *) + strlen(archive) + 1;
    for (int i = 0; i < n; i++)
        bytes += strlen(paths[i]) + 1;
    char **names = (char **)heap_alloc(HEAP_FILES, bytes);
    if (!names)
        return NULL;
    char *p = (char *)(names + n + 2);
    for (int i = 0; i <= n; i++) {
        const char *s = i ? paths[i - 1] : archive;
        size_t len = strlen(s) + 1;
        memcpy(p, s, len);
        names[i] = p;
        p += len;
    }
    names[n + 1] = NULL;
    return names;
}

void tar_create(shell_t *sh, const char *archive, const char *dir, bool gz, bool force,
                bool verbose, int n, char **paths) {
    char arc[PATH_LEN], target[PATH_LEN];
    if (!path_resolve(sh->cwd, archive, arc, sizeof(arc)) ||
        !path_resolve(sh->cwd, dir ? dir : ".", target, sizeof(target))) {
        shell_printf(sh, "tar: path too long\n");
        return;
    }
    if (!sdcard_mount(sh))
        return;
    FILINFO fi;
    if (strcmp(target, "/") && (f_stat(target, &fi) != FR_OK || !(fi.fattrib & AM_DIR))) {
        shell_printf(sh, "tar: %s: no such directory\n", dir ? dir : target);
        return;
    }
    char part[PATH_LEN + sizeof(PART)];
    snprintf(part, sizeof(part), "%s%s", arc, PART);
    if (!strcmp(arc, "/") || !files_write_allowed(sh, arc) || !files_write_allowed(sh, part)) {
        shell_printf(sh, "tar: %s: not allowed to change this path\n", archive);
        return;
    }
    if (f_stat(arc, &fi) == FR_OK) {        // checked again at the end (files_install_part())
        if (fi.fattrib & AM_DIR) {
            shell_printf(sh, "tar: %s: is a directory\n", archive);
            return;
        }
        if (!force) {
            shell_printf(sh, "tar: %s: already exists (-f replaces it)\n", archive);
            return;
        }
    }
    size_t need = sizeof(tarc_t) + (gz ? GZW_HEAP : 0), free_now = heap_free();
    if (free_now < need + HEAP_KEEP) {
        shell_printf(sh, "tar: not enough memory: needs %u KB and keeps %u KB free; %u KB free\n",
                     (unsigned)(need / 1024), HEAP_KEEP / 1024, (unsigned)(free_now / 1024));
        return;
    }
    const char *other = bigjob_try_begin("tar");
    if (other) {
        shell_printf(sh, BIGJOB_BUSY_FMT, other);
        return;
    }
    tarc_t *j = heap_zalloc(HEAP_FILES, sizeof(*j));
    char **names = j ? copy_names(archive, n, paths) : NULL;
    if (!names || (gz && !gzw_init(&j->gzw, GZW_LEVEL))) {
        if (j && gz)
            gzw_end(&j->gzw);
        heap_release(HEAP_FILES, (void *)names);
        heap_release(HEAP_FILES, j);
        bigjob_end();
        shell_printf(sh, "tar: out of memory\n");
        return;
    }
    FRESULT fr = f_open(&j->out, part, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        shell_printf(sh, "tar: %s: %s\n", path_basename(part), files_error((int)fr));
        if (gz)
            gzw_end(&j->gzw);
        heap_release(HEAP_FILES, (void *)names);
        heap_release(HEAP_FILES, j);
        bigjob_end();
        return;
    }
    j->gz = gz;
    j->force = force;
    j->verbose = verbose;
    j->names = names;
    j->arg = names[0];
    j->paths = names + 1;
    j->n = n;
    strcpy(j->arc, arc);
    strcpy(j->part, part);
    strcpy(j->target, target);
    j->synced_at = time_us_64();
    files_hold();
    shell_run_job(sh, create_step, j);
}
