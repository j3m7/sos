// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// tar tf and tar xf (docs/design/archives.md §6): an archive on the card, plain or gzipped (known
// by its first two bytes, decoded in the same pass by files/gz.h), read by files/tar_parse.h as a
// shell job.
//
// Each step does a bounded amount of work for STEP_US: a 4 KB read from the card, inflate into a
// 4 KB buffer, a header, or a piece of an entry's data written to its file. `tf` prints a line per
// entry through shell_write(), as fast as shell_write_space() lets it, so `| grep` and `| more`
// work. `xf` writes each file to <name>.part and renames it into place at its end
// (files_install_part()); Ctrl-C, a read or write error, or a bad header stops the job and
// deletes the .part, and what was extracted stays.
//
// The archive comes from elsewhere, so every name passes §6.3's rules before anything is created:
// tar_name_clean() (leading '/' removed and said once; "..", FAT's forbidden names and lengths
// refused), the joined path checked to stay under the target, files_write_allowed() on every
// create, never the archive itself. FAT ignores case: a name that differs only in case from one
// written in this run is refused, found by asking FatFs for the name it has stored (a stat per
// component, only when the path already exists) and looking that up among hashes of the paths
// this run created. The same name twice: the later one wins, as Unix tar. An existing file not
// made by this run is not replaced without -f.
//
// Dates: FatFs is built with FF_USE_CHMOD 1 (config/fatfs/ffconf.h), so extracted files and
// directories get the header's date with f_utime(). The card keeps UTC (time/README.md), as the
// header does, so the date is stored as it is.
//
// Memory: the job (two FatFs files, the parser with its name and pax buffers, two 4 KB buffers,
// paths: ~22 KB) and, for a gzipped archive, zlib's ~40 KB; the hashes of the paths created
// (8 bytes each, grown as needed, up to SEEN_MAX). Refused when that would leave less than
// HEAP_KEEP free; and one at a time across the shells (files/bigjob.h).

#include "files/tar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "pico/stdlib.h"

#include "ff.h"

#include "core/heap.h"
#include "core/heaptag.h"
#include "files/bigjob.h"
#include "files/files.h"
#include "files/gz.h"
#include "files/path.h"
#include "files/sdcard.h"
#include "files/tar_parse.h"
#include "time/timeconv.h"

#define PATH_LEN    256             // FatFs long names are up to 255 characters
#define STEP_US     20000           // work per job step before letting the main loop run
#define BUF         4096            // the read buffer, and gzip's output buffer
#define HEAP_KEEP   (64u * 1024u)   // heap left free while a job runs (a TLS handshake)
#define SEEN_MIN    256             // hashes of created paths: the first allocation
#define SEEN_MAX    8192            // and the most kept (64 KB)
#define FAT32_MAX   0xffffffffull   // the largest file FAT32 holds
#define LINE_MAX    (2 * TAR_NAME_MAX + 96)
#define PART        ".part"

typedef struct {
    bool list, force, verbose;
    bool gz;                        // the archive is gzipped
    bool eof;                       // the archive has been read to its end
    bool out_open;                  // the current entry's .part is open
    bool replace;                   // and it may replace an existing file at its end
    bool said_strip, said_seen;     // "removing leading /" and the hashes' limit said once
    FIL in, out;
    const char *arg;                // the archive as typed
    char arc[PATH_LEN];             // the archive, absolute
    char target[PATH_LEN];          // -C, or the working directory
    char dest[PATH_LEN];            // the current entry's path
    char part[PATH_LEN + sizeof(PART)];
    char known_dir[PATH_LEN];       // a directory known to exist (a file's parent, last time)
    char rel[TAR_NAME_MAX];         // the current entry's name, cleaned (tar_name_clean())
    int64_t mtime;                  // the current entry's date
    unsigned files, dirs, skipped;
    uint64_t *seen;                 // hashes of the paths created, sorted
    size_t seen_n, seen_cap;
    uint64_t synced_at;
    const uint8_t *cur;             // tar bytes not yet given to the parser
    size_t cur_len;
    size_t in_pos, in_len;          // gzipped: in_buf[in_pos..in_len) read, not yet decoded
    size_t line_len;                // tf: a line in `line` waiting for room
    gz_t gzs;
    tar_t tar;
    char line[LINE_MAX];
    uint8_t in_buf[BUF];
    uint8_t gz_buf[BUF];
} tarjob_t;

// --- output ------------------------------------------------------------------------------------

// Append `s` to j->line at *len, control characters shown as '?' (names come from the archive).
static void add(tarjob_t *j, size_t *len, const char *s, bool name) {
    for (; *s && *len < sizeof(j->line) - 2; s++) {
        unsigned char c = (unsigned char)*s;
        j->line[(*len)++] = name && (c < 0x20 || c == 0x7f) ? '?' : (char)c;
    }
    j->line[*len] = '\0';
}

// "tar: <name>: <why>\n", the name in full (shell_printf() cuts at 255 bytes).
static void say(shell_t *sh, tarjob_t *j, const char *name, const char *why) {
    size_t len = 0;
    add(j, &len, "tar: ", false);
    add(j, &len, name, true);
    add(j, &len, ": ", false);
    add(j, &len, why, false);
    add(j, &len, "\n", false);
    shell_write(sh, j->line, len);
}

static void skip(shell_t *sh, tarjob_t *j, const char *name, const char *why) {
    say(sh, j, name, why);
    j->skipped++;
}

// --- paths created in this run ---------------------------------------------------------------

static uint64_t path_hash(const char *s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *s; s++)
        h = (h ^ (uint8_t)*s) * 0x100000001b3ull;
    return h;
}

// Where `h` is in the sorted j->seen, or would go.
static size_t seen_at(const tarjob_t *j, uint64_t h) {
    size_t lo = 0, hi = j->seen_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (j->seen[mid] < h)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static bool seen_has(const tarjob_t *j, const char *path) {
    uint64_t h = path_hash(path);
    size_t i = seen_at(j, h);
    return i < j->seen_n && j->seen[i] == h;
}

static void seen_add(shell_t *sh, tarjob_t *j, const char *path) {
    uint64_t h = path_hash(path);
    size_t i = seen_at(j, h);
    if (i < j->seen_n && j->seen[i] == h)
        return;
    if (j->seen_n == j->seen_cap) {
        size_t cap = j->seen_cap ? 2 * j->seen_cap : SEEN_MIN;
        uint64_t *p = NULL;
        if (cap <= SEEN_MAX && heap_free() >= cap * sizeof(*p) + HEAP_KEEP)
            p = heap_realloc(HEAP_FILES, j->seen, cap * sizeof(*p));
        if (!p) {
            if (!j->said_seen)
                shell_printf(sh, "tar: too many names to remember: from here an existing file "
                                 "is replaced only with -f, and case clashes aren't caught\n");
            j->said_seen = true;
            return;
        }
        j->seen = p;
        j->seen_cap = cap;
    }
    memmove(j->seen + i + 1, j->seen + i, (j->seen_n - i) * sizeof(*j->seen));
    j->seen[i] = h;
    j->seen_n++;
}

// The name FatFs has stored for the existing `path` (under the target), in `out`: each component
// below the target as the card spells it.
static void stored_name(const tarjob_t *j, const char *path, char *out) {
    size_t base = strcmp(j->target, "/") ? strlen(j->target) : 0;
    strcpy(out, path);
    for (size_t i = base + 1; out[i - 1];) {
        size_t len = strcspn(out + i, "/");
        char save = out[i + len];
        out[i + len] = '\0';
        FILINFO fi;
        if (f_stat(out, &fi) == FR_OK && strlen(fi.fname) == len)
            memcpy(out + i, fi.fname, len);
        out[i + len] = save;
        i += len + (save ? 1 : 0);
        if (!save)
            break;
    }
}

// --- dates -------------------------------------------------------------------------------------

// The entry's date on `path` (UTC, as the card keeps it). Dates FAT can't hold are left as the
// extraction time.
static void set_date(const char *path, int64_t t) {
    timeconv_cal_t c;
    timeconv_to_cal(t, &c);
    if (c.year < 1980 || c.year > 2107)
        return;
    FILINFO fi;
    memset(&fi, 0, sizeof(fi));
    fi.fdate = (uint16_t)((c.year - 1980) << 9 | c.month << 5 | c.day);
    fi.ftime = (uint16_t)(c.hour << 11 | c.min << 5 | c.sec / 2);
    f_utime(path, &fi);
}

// --- extracting --------------------------------------------------------------------------------

// The directories above j->dest, created where missing. False after saying why (skipped).
static bool make_parents(shell_t *sh, tarjob_t *j, const char *name) {
    char dir[PATH_LEN];
    strcpy(dir, j->dest);
    char *slash = strrchr(dir, '/');
    *slash = '\0';
    if (!strcmp(dir, j->known_dir) || strlen(dir) <= strlen(j->target))
        return true;
    size_t base = strcmp(j->target, "/") ? strlen(j->target) : 0;
    for (char *p = dir + base + 1;; p++) {
        if (*p && *p != '/')
            continue;
        char save = *p;
        *p = '\0';
        FILINFO fi;
        FRESULT fr = f_stat(dir, &fi);
        if (fr == FR_OK && !(fi.fattrib & AM_DIR)) {
            skip(sh, j, name, "a file is in the way of its directory");
            return false;
        }
        if (fr != FR_OK) {
            if (!files_write_allowed(sh, dir)) {
                skip(sh, j, name, "not allowed to change this path");
                return false;
            }
            fr = f_mkdir(dir);
            if (fr != FR_OK) {
                skip(sh, j, name, files_error((int)fr));
                return false;
            }
            seen_add(sh, j, dir);
            j->dirs++;
        }
        *p = save;
        if (!save)
            break;
    }
    strcpy(j->known_dir, dir);
    return true;
}

// The archive told of a file or directory: decide, and for a file open its .part.
static void start_entry(shell_t *sh, tarjob_t *j) {
    const tar_entry_t *e = &j->tar.entry;
    const char *name = e->name;
    char kind_buf[24];
    const char *why = e->refused;
    if (!why)
        why = tar_kind_skipped(e->kind, e->type, kind_buf, sizeof(kind_buf));
    bool stripped = false;
    if (!why)
        why = tar_name_clean(name, j->rel, sizeof(j->rel), &stripped);
    if (stripped && !j->said_strip) {
        shell_printf(sh, "tar: removing leading / from names\n");
        j->said_strip = true;
    }
    if (why) {
        skip(sh, j, name, why);
        return;
    }
    bool dir = e->kind == TAR_DIR;
    if (!j->rel[0]) {
        if (!dir)
            skip(sh, j, name, "empty name");
        return;                             // "./": the target itself
    }
    // Joined to the target, the path must stay under it. With ".." refused and no links on
    // FAT that holds by construction; checked all the same.
    char norm[PATH_LEN];
    size_t tlen = strlen(j->target);
    if (!path_join(j->target, j->rel, j->dest, sizeof(j->dest)) ||
        strlen(j->dest) + sizeof(PART) > sizeof(j->part)) {
        skip(sh, j, name, "name too long");
        return;
    }
    if (!path_resolve("/", j->dest, norm, sizeof(norm)) || strcmp(norm, j->dest) ||
        (tlen > 1 && (strncmp(norm, j->target, tlen) || norm[tlen] != '/'))) {
        skip(sh, j, name, "unsafe path");
        return;
    }
    strcpy(j->part, j->dest);
    strcat(j->part, PART);
    if (!strcasecmp(j->dest, j->arc) || !strcasecmp(j->part, j->arc)) {
        skip(sh, j, name, "is the archive itself");
        return;
    }
    if (!dir && e->size > FAT32_MAX && !sdcard_exfat()) {
        skip(sh, j, name, "too large for FAT32");
        return;
    }
    if (!files_write_allowed(sh, j->dest) || (!dir && !files_write_allowed(sh, j->part))) {
        skip(sh, j, name, "not allowed to change this path");
        return;
    }
    if (!make_parents(sh, j, name))
        return;

    FILINFO fi;
    FRESULT fr;
    j->replace = false;
    if (f_stat(j->dest, &fi) == FR_OK) {
        char stored[PATH_LEN];
        stored_name(j, j->dest, stored);
        // Made by this run: as spelled here, or as the card spells it (a directory on the
        // way may have had another case already).
        bool ours = seen_has(j, j->dest) || seen_has(j, stored);
        if (strcmp(stored, j->dest) && seen_has(j, stored)) {
            char msg[PATH_LEN + 32];
            snprintf(msg, sizeof(msg), "same name as %s on FAT",
                     stored + (tlen > 1 ? tlen + 1 : 1));
            skip(sh, j, name, msg);
            return;
        }
        bool is_dir = (fi.fattrib & AM_DIR) != 0;
        if (dir) {
            if (!is_dir)
                skip(sh, j, name, "a file is in the way");
            return;                         // an existing directory: merged
        }
        if (is_dir) {
            skip(sh, j, name, "is a directory");
            return;
        }
        if (!ours && !j->force) {
            skip(sh, j, name, "exists (-f replaces it)");
            return;
        }
        j->replace = true;
    } else if (dir) {
        fr = f_mkdir(j->dest);
        if (fr != FR_OK) {
            skip(sh, j, name, files_error((int)fr));
            return;
        }
        seen_add(sh, j, j->dest);
        set_date(j->dest, e->mtime);
        j->dirs++;
        if (j->verbose)
            say(sh, j, j->rel, "directory");
        return;
    }
    fr = f_open(&j->out, j->part, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        skip(sh, j, name, files_error((int)fr));
        return;
    }
    j->out_open = true;
    j->mtime = e->mtime;
    j->synced_at = time_us_64();
}

// The current file's data has all been written: close it, date it, put it in place.
static void end_file(shell_t *sh, tarjob_t *j) {
    j->out_open = false;
    FRESULT fr = f_close(&j->out);
    if (fr != FR_OK) {
        f_unlink(j->part);
        skip(sh, j, j->tar.entry.name, files_error((int)fr));
        return;
    }
    set_date(j->part, j->mtime);
    const char *why;
    files_install_t r = files_install_part(sh, "tar", "file", j->part, j->dest, j->replace, &why);
    if (r != FILES_INSTALLED) {
        if (r == FILES_NOT_INSTALLED)
            f_unlink(j->part);
        skip(sh, j, j->tar.entry.name, why);
        return;
    }
    seen_add(sh, j, j->dest);
    j->files++;
    if (j->verbose) {
        size_t len = 0;
        add(j, &len, j->rel, true);
        add(j, &len, "\n", false);
        shell_write(sh, j->line, len);
    }
}

// --- listing -----------------------------------------------------------------------------------

// tf: the entry's line into j->line: type, size, date (UTC, as `ls`), name, a link's target,
// and why `xf` would skip it.
static void list_entry(tarjob_t *j) {
    const tar_entry_t *e = &j->tar.entry;
    static const char kinds[] = {'-', 'd', 'h', 'l', 'c', 'b', 'p', '?'};
    timeconv_cal_t c;
    timeconv_to_cal(e->mtime, &c);
    size_t len = (size_t)snprintf(j->line, sizeof(j->line),
                                  "%c %12llu  %04d-%02d-%02d %02d:%02d UTC  ", kinds[e->kind],
                                  (unsigned long long)e->size, c.year, c.month, c.day, c.hour,
                                  c.min);
    if (len >= sizeof(j->line))
        len = sizeof(j->line) - 1;
    add(j, &len, e->name, true);
    if (e->link[0]) {
        add(j, &len, " -> ", false);
        add(j, &len, e->link, true);
    }
    char kind_buf[24];
    bool stripped;
    const char *why = e->refused;
    if (!why)
        why = tar_kind_skipped(e->kind, e->type, kind_buf, sizeof(kind_buf));
    if (!why)
        why = tar_name_clean(e->name, j->rel, sizeof(j->rel), &stripped);
    if (!why && !j->rel[0] && e->kind != TAR_DIR)
        why = "empty name";
    if (why) {
        add(j, &len, "  (", false);
        add(j, &len, why, false);
        add(j, &len, ")", false);
    }
    add(j, &len, "\n", false);
    j->line_len = len;
}

// Print the waiting line when the channel has room for it. False while it waits.
static bool flush_line(shell_t *sh, tarjob_t *j) {
    size_t need = j->line_len + 1 < 1024 ? j->line_len + 1 : 1024;   // "\n" becomes "\r\n"
    if ((size_t)shell_write_space(sh) < need)
        return false;
    shell_write(sh, j->line, j->line_len);
    j->line_len = 0;
    return true;
}

// --- the job -----------------------------------------------------------------------------------

static bool job_end(tarjob_t *j) {
    if (j->gz)
        gz_end(&j->gzs);
    f_close(&j->in);
    bigjob_end();
    files_release();
    heap_release(HEAP_FILES, j->seen);
    heap_release(HEAP_FILES, j);
    return false;
}

// The end, cleanly or not (`why`): the .part deleted, the summary.
static bool finish(shell_t *sh, tarjob_t *j, const char *why) {
    if (j->out_open) {
        f_close(&j->out);
        f_unlink(j->part);
        j->out_open = false;
    }
    if (!j->list)
        shell_printf(sh, "%u files, %u directories, %u skipped\n", j->files, j->dirs, j->skipped);
    if (why)
        shell_printf(sh, "stopped: %s\n", why);
    return job_end(j);
}

static bool bad_header(shell_t *sh, tarjob_t *j) {
    char why[64];
    if (!strcmp(j->tar.error, "bad header") || !strcmp(j->tar.error, "bad pax header"))
        snprintf(why, sizeof(why), "%s at %llu", j->tar.error,
                 (unsigned long long)j->tar.error_at);
    else
        snprintf(why, sizeof(why), "%s", j->tar.error);
    return finish(sh, j, why);
}

// More tar bytes into j->cur: a read, or (gzipped) a decode. Returns false when the job is over
// (and ended), else true.
static bool refill(shell_t *sh, tarjob_t *j) {
    if (j->gz && (j->in_pos < j->in_len || gz_pending(&j->gzs))) {
        const uint8_t *in = j->in_buf + j->in_pos;
        size_t left = j->in_len - j->in_pos;
        bool ok = gz_feed(&j->gzs, &in, &left, j->gz_buf, sizeof(j->gz_buf), &j->cur_len);
        j->in_pos = j->in_len - left;
        j->cur = j->gz_buf;
        if (!ok && !j->cur_len)
            return finish(sh, j, j->gzs.error);
        return true;
    }
    if (j->eof) {
        if (j->gz && !gz_finish(&j->gzs))
            return finish(sh, j, j->gzs.error);
        if (!tar_finish(&j->tar))
            return bad_header(sh, j);
        return finish(sh, j, NULL);         // one zero block and the end (GNU accepts it)
    }
    UINT br;
    FRESULT fr = f_read(&j->in, j->in_buf, sizeof(j->in_buf), &br);
    if (fr != FR_OK)
        return finish(sh, j, files_error((int)fr));
    j->eof = br == 0;
    if (j->gz) {
        j->in_pos = 0;
        j->in_len = br;
    } else {
        j->cur = j->in_buf;
        j->cur_len = br;
    }
    return true;
}

// One piece of work. Returns false when the job is over (and ended).
static bool advance(shell_t *sh, tarjob_t *j) {
    if (!j->cur_len) {
        if (j->gz && j->gzs.state == GZ_FAILED)
            return finish(sh, j, j->gzs.error);
        return refill(sh, j);
    }
    switch (tar_feed(&j->tar, &j->cur, &j->cur_len)) {
    case TAR_MORE:
        return true;
    case TAR_HEADER:
        if (j->list)
            list_entry(j);
        else
            start_entry(sh, j);
        if (j->out_open && tar_entry_done(&j->tar))
            end_file(sh, j);                // an empty file
        return true;
    case TAR_DATA:
        if (j->out_open) {
            UINT bw;
            FRESULT fr = f_write(&j->out, j->tar.data, (UINT)j->tar.data_len, &bw);
            if (fr == FR_OK && bw != j->tar.data_len)
                return finish(sh, j, "card full");
            if (fr == FR_OK)
                fr = files_sync_if_due(&j->out, &j->synced_at);
            if (fr != FR_OK)
                return finish(sh, j, files_error((int)fr));
            if (tar_entry_done(&j->tar))
                end_file(sh, j);
        }
        return true;
    case TAR_END:
        return finish(sh, j, NULL);         // what follows the end is ignored
    case TAR_ERROR:
        break;
    }
    return bad_header(sh, j);
}

static bool tar_step(shell_t *sh, void *arg, bool cancel) {
    tarjob_t *j = arg;
    if (cancel)
        return finish(sh, j, "Ctrl-C");
    uint64_t t0 = time_us_64();
    do {                                    // at least one piece of work a step
        if (j->line_len && !flush_line(sh, j))
            return true;                    // tf: wait for the terminal
        if (!advance(sh, j))
            return false;
    } while (time_us_64() - t0 < STEP_US);
    return true;
}

// --- the command -------------------------------------------------------------------------------

static void usage(shell_t *sh) {
    shell_printf(sh, "usage: tar tf <archive>\n"
                     "       tar xf <archive> [-C <dir>] [-f]\n");
}

// The mode word: t or x, f, and z or v, in any order, with or without a leading '-'.
static bool parse_mode(const char *w, bool *list, bool *verbose) {
    bool t = false, x = false, f = false;
    if (*w == '-')
        w++;
    if (!*w)
        return false;
    for (; *w; w++) {
        if (*w == 't')
            t = true;
        else if (*w == 'x')
            x = true;
        else if (*w == 'f')
            f = true;
        else if (*w == 'v')
            *verbose = true;
        else if (*w != 'z')
            return false;
    }
    *list = t;
    return f && t != x;
}

static void run_tar(shell_t *sh, int argc, char **argv) {
    bool list = false, verbose = false, force = false;
    const char *dir = NULL;
    if (argc < 3 || !parse_mode(argv[1], &list, &verbose) || !argv[2][0]) {
        usage(sh);
        return;
    }
    for (int i = 3; i < argc; i++) {
        if (!list && !strcmp(argv[i], "-C") && i + 1 < argc && argv[i + 1][0] && !dir) {
            dir = argv[++i];
        } else if (!list && !strcmp(argv[i], "-f")) {
            force = true;
        } else {
            usage(sh);
            return;
        }
    }
    char arc[PATH_LEN], target[PATH_LEN];
    if (!path_resolve(sh->cwd, argv[2], arc, sizeof(arc)) ||
        !path_resolve(sh->cwd, dir ? dir : ".", target, sizeof(target))) {
        shell_printf(sh, "tar: path too long\n");
        return;
    }
    if (!sdcard_mount(sh))
        return;
    FILINFO fi;
    if (!list && (f_stat(target, &fi) != FR_OK || !(fi.fattrib & AM_DIR)) && strcmp(target, "/")) {
        shell_printf(sh, "tar: %s: no such directory\n", dir ? dir : target);
        return;
    }
    size_t need = sizeof(tarjob_t), free_now = heap_free();
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
    tarjob_t *j = heap_zalloc(HEAP_FILES, sizeof(*j));
    if (!j) {
        bigjob_end();
        shell_printf(sh, "tar: out of memory\n");
        return;
    }
    FRESULT fr = f_open(&j->in, arc, FA_READ);
    UINT br = 0;
    if (fr == FR_OK && (fr = f_read(&j->in, j->in_buf, sizeof(j->in_buf), &br)) != FR_OK)
        f_close(&j->in);
    if (fr != FR_OK) {
        shell_printf(sh, "tar: %s: %s\n", argv[2], files_error((int)fr));
        heap_release(HEAP_FILES, j);
        bigjob_end();
        return;
    }
    j->gz = br >= 2 && j->in_buf[0] == 0x1f && j->in_buf[1] == 0x8b;
    free_now = heap_free();
    if (j->gz && (free_now < GZ_HEAP + HEAP_KEEP || !gz_init(&j->gzs))) {
        shell_printf(sh, "tar: not enough memory: gzip needs %u KB more and keeps %u KB free; "
                         "%u KB free\n", GZ_HEAP / 1024, HEAP_KEEP / 1024,
                     (unsigned)(free_now / 1024));
        gz_end(&j->gzs);
        f_close(&j->in);
        heap_release(HEAP_FILES, j);
        bigjob_end();
        return;
    }
    if (j->gz) {
        j->in_len = br;
    } else {
        j->cur = j->in_buf;
        j->cur_len = br;
    }
    j->eof = br == 0;
    j->list = list;
    j->force = force;
    j->verbose = verbose;
    j->arg = argv[2];
    strcpy(j->arc, arc);
    strcpy(j->target, target);
    tar_init(&j->tar);
    files_hold();
    shell_run_job(sh, tar_step, j);
}

const shell_cmd_t tar_commands[] = {
    {"tar", NULL, run_tar, NULL, SHELL_PATH},
    {0},
};

void tar_help(shell_t *sh) {
    shell_printf(sh, "tar tf <archive>  list a tar archive (.tar, .tar.gz, .tgz)\n");
    shell_printf(sh, "tar xf <archive> [-C <dir>] [-f]  extract it (-f replaces existing files)\n");
}
