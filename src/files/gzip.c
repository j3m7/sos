// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// gunzip and zcat (docs/design/archives.md §5.2, §5.3): gzip files on the card, decoded by
// files/gz.h (zlib's inflate) as shell jobs.
//
// Each step does a bounded amount of work for STEP_US: a 4 KB read from the card, inflate into a
// 4 KB buffer, or that buffer's write (gunzip: to <name>.part; zcat: to the terminal, as fast as
// shell_write_space() lets it, like cat). A small input that inflates to a lot (a "zip bomb")
// only makes many steps; the card's free space is the limit, and a full card stops gunzip with
// "card full". The output is renamed from <name>.part to <name> only when the input ended
// cleanly; any error, Ctrl-C or the session ending deletes the .part. The input is deleted after
// success unless -k. An existing <name> is not replaced without -f, and with -f it is moved
// aside while the output takes its name (files_install_part()).
//
// Memory: the job (two FatFs files, three paths, the two buffers: ~10 KB) and zlib's ~40 KB.
// Refused when that would leave less than HEAP_KEEP free, so time sync, a TLS handshake and an
// `http` request still fit; and one at a time across the shells (files/bigjob.h).

#include "files/gzip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"

#include "ff.h"

#include "core/heap.h"
#include "core/heaptag.h"
#include "files/bigjob.h"
#include "files/files.h"
#include "files/gz.h"
#include "files/path.h"
#include "files/sdcard.h"

#define PATH_LEN    256             // FatFs long names are up to 255 characters
#define STEP_US     20000           // work per job step before letting the main loop run
#define GZ_BUF      4096            // input and output buffers
#define HEAP_KEEP   (64u * 1024u)   // heap left free while a job runs (a TLS handshake)
#define PART        ".part"

typedef struct {
    const char *cmd;                // "gunzip" or "zcat": messages, and the bigjob slot
    bool zcat, keep, force;
    char **names;                   // the file arguments as typed (copies), names[n] NULL
    int n, next;                    // how many; the next to start

    // The file being decoded (open).
    bool open;
    const char *arg;                // as typed, for messages
    FIL in, out;
    char out_path[PATH_LEN], part_path[PATH_LEN], in_path[PATH_LEN];
    gz_t gz;
    const char *failed;             // zcat: the decoder's error, said once its output is printed
    bool eof;                       // the input file has been read to its end
    size_t in_pos, in_len;          // in_buf[in_pos..in_len): read, not yet decoded
    size_t out_pos, out_len;        // out_buf[out_pos..out_len): decoded, not yet written
    uint64_t t0, synced_at;
    uint8_t in_buf[GZ_BUF];
    uint8_t out_buf[GZ_BUF];
} gzjob_t;

// --- one file ------------------------------------------------------------------------------------

// Close the file being decoded. Without `keep_output`, delete the .part.
static FRESULT close_file(gzjob_t *j, bool keep_output) {
    f_close(&j->in);
    FRESULT fr = FR_OK;
    if (!j->zcat) {
        fr = f_close(&j->out);
        if (!keep_output || fr != FR_OK)
            f_unlink(j->part_path);
    }
    gz_end(&j->gz);
    j->open = false;
    return fr;
}

static void file_failed(shell_t *sh, gzjob_t *j, const char *why) {
    close_file(j, false);
    shell_printf(sh, "%s%s: %s: %s\n", j->zcat ? "\n" : "", j->cmd, j->arg, why);
}

// The input ended cleanly: gunzip renames the .part into place and deletes the input.
static void file_done(shell_t *sh, gzjob_t *j) {
    bool zeros = j->gz.trailing_zeros;
    uint64_t bytes = j->gz.out_total;
    FRESULT fr = close_file(j, true);
    if (j->zcat)
        return;
    if (fr != FR_OK) {
        shell_printf(sh, "gunzip: %s: %s\n", j->arg, files_error((int)fr));
        return;
    }
    const char *why;
    files_install_t r = files_install_part(sh, "gunzip", "output", j->part_path, j->out_path,
                                           j->force, &why);
    if (r != FILES_INSTALLED) {
        if (r == FILES_NOT_INSTALLED)
            f_unlink(j->part_path);
        shell_printf(sh, "gunzip: %s: %s\n", j->out_path, why);
        return;                             // the input is kept
    }
    if (zeros)
        shell_printf(sh, "gunzip: %s: ignoring trailing zeros\n", j->arg);
    if (!j->keep && (fr = f_unlink(j->in_path)) != FR_OK)
        shell_printf(sh, "gunzip: %s: not deleted: %s\n", j->arg, files_error((int)fr));
    uint64_t us = time_us_64() - j->t0;
    shell_printf(sh, "%s: %llu bytes in %llu ms (%llu KB/s)\n", path_basename(j->out_path),
                 (unsigned long long)bytes, (unsigned long long)(us / 1000),
                 (unsigned long long)(us ? bytes * 1000000 / 1024 / us : 0));
}

// For gunzip: may this shell change `path`? Says so if not.
static bool may_write(shell_t *sh, const char *path) {
    if (files_write_allowed(sh, path))
        return true;
    shell_printf(sh, "gunzip: %s: not allowed to change this path\n", path);
    return false;
}

// Open the next file and its output. False after saying why (the job goes on to the next).
static bool start_file(shell_t *sh, gzjob_t *j) {
    j->arg = j->names[j->next++];
    if (!path_resolve(sh->cwd, j->arg, j->in_path, sizeof(j->in_path))) {
        shell_printf(sh, "%s: %s: path too long\n", j->cmd, j->arg);
        return false;
    }
    if (!sdcard_mount(sh))
        return false;
    if (!j->zcat) {
        if (!gz_output_name(j->in_path, j->out_path, sizeof(j->out_path))) {
            shell_printf(sh, "gunzip: %s: unknown suffix (.gz or .tgz)\n", j->arg);
            return false;
        }
        if (strlen(j->out_path) + strlen(PART) >= sizeof(j->part_path)) {
            shell_printf(sh, "gunzip: %s: path too long\n", j->arg);
            return false;
        }
        strcpy(j->part_path, j->out_path);
        strcat(j->part_path, PART);
        if (!may_write(sh, j->out_path) || !may_write(sh, j->part_path) ||
            (!j->keep && !may_write(sh, j->in_path)))
            return false;
        FILINFO fi;                         // checked again at the end (files_install_part())
        bool exists = f_stat(j->out_path, &fi) == FR_OK;
        if (exists && (fi.fattrib & AM_DIR)) {
            shell_printf(sh, "gunzip: %s: is a directory\n", path_basename(j->out_path));
            return false;
        }
        if (exists && !j->force) {
            shell_printf(sh, "gunzip: %s: already exists (-f replaces it)\n",
                         path_basename(j->out_path));
            return false;
        }
    }
    FRESULT fr = f_open(&j->in, j->in_path, FA_READ);
    if (fr != FR_OK) {
        shell_printf(sh, "%s: %s: %s\n", j->cmd, j->arg, files_error((int)fr));
        return false;
    }
    if (!j->zcat && (fr = f_open(&j->out, j->part_path, FA_WRITE | FA_CREATE_ALWAYS)) != FR_OK) {
        shell_printf(sh, "gunzip: %s: %s\n", path_basename(j->part_path), files_error((int)fr));
        f_close(&j->in);
        return false;
    }
    if (!gz_init(&j->gz)) {
        j->open = true;
        file_failed(sh, j, j->gz.error);
        return false;
    }
    j->open = true;
    j->failed = NULL;
    j->eof = false;
    j->in_pos = j->in_len = j->out_pos = j->out_len = 0;
    j->t0 = j->synced_at = time_us_64();
    return true;
}

// Write what is decoded. gunzip: all of it to the .part. zcat: what the terminal takes now
// (half its space: "\n" becomes "\r\n"). False after an error (the file is closed then).
static bool put_out(shell_t *sh, gzjob_t *j) {
    size_t n = j->out_len - j->out_pos;
    if (j->zcat) {
        int space = shell_write_space(sh) / 2;
        if (space < 64)
            return true;                    // wait for the client to read
        if (n > (size_t)space)
            n = (size_t)space;
        shell_write(sh, (const char *)j->out_buf + j->out_pos, n);
        j->out_pos += n;
        return true;
    }
    UINT bw;
    FRESULT fr = f_write(&j->out, j->out_buf + j->out_pos, (UINT)n, &bw);
    if (fr == FR_OK && bw != n) {
        file_failed(sh, j, "card full");
        return false;
    }
    if (fr == FR_OK)
        fr = files_sync_if_due(&j->out, &j->synced_at);
    if (fr != FR_OK) {
        file_failed(sh, j, files_error((int)fr));
        return false;
    }
    j->out_pos += n;
    return true;
}

// One bounded piece of work on the open file: write, read, or decode. Returns false when zcat
// must wait for the terminal.
static bool advance(shell_t *sh, gzjob_t *j) {
    if (j->out_pos < j->out_len) {
        size_t before = j->out_pos;
        if (!put_out(sh, j))
            return true;                    // failed: the file is closed
        return j->out_pos != before;
    }
    if (j->failed) {
        file_failed(sh, j, j->failed);
        return true;
    }
    if (j->in_pos == j->in_len && !j->eof) {
        UINT br;
        FRESULT fr = f_read(&j->in, j->in_buf, sizeof(j->in_buf), &br);
        if (fr != FR_OK) {
            file_failed(sh, j, files_error((int)fr));
            return true;
        }
        j->in_pos = 0;
        j->in_len = br;
        j->eof = br == 0;
        return true;
    }
    if (j->in_pos < j->in_len || gz_pending(&j->gz)) {
        const uint8_t *in = j->in_buf + j->in_pos;
        size_t left = j->in_len - j->in_pos;
        bool ok = gz_feed(&j->gz, &in, &left, j->out_buf, sizeof(j->out_buf), &j->out_len);
        j->in_pos = j->in_len - left;
        j->out_pos = 0;
        if (!ok && j->zcat)
            j->failed = j->gz.error;        // after what came before the error
        else if (!ok)
            file_failed(sh, j, j->gz.error);
        return true;
    }
    if (gz_finish(&j->gz))
        file_done(sh, j);
    else
        file_failed(sh, j, j->gz.error);
    return true;
}

// --- the job -------------------------------------------------------------------------------------

static bool job_end(gzjob_t *j) {
    bigjob_end();
    files_release();
    heap_release(HEAP_FILES, (void *)j->names);
    heap_release(HEAP_FILES, j);
    return false;
}

static bool gz_step(shell_t *sh, void *arg, bool cancel) {
    gzjob_t *j = arg;
    if (cancel) {
        if (j->open) {
            close_file(j, false);
            if (!j->zcat)
                shell_printf(sh, "gunzip: stopped; %s removed\n", path_basename(j->part_path));
        }
        return job_end(j);
    }
    uint64_t t0 = time_us_64();
    do {                                    // at least one piece of work a step
        if (!j->open) {
            if (j->next == j->n)
                return job_end(j);
            start_file(sh, j);
        } else if (!advance(sh, j)) {
            return true;                    // zcat: the terminal is full
        }
    } while (time_us_64() - t0 < STEP_US);
    return true;
}

// Copy the file arguments (argv lives only as long as the command's line).
static char **copy_names(int n, char **argv) {
    size_t bytes = (size_t)(n + 1) * sizeof(char *);
    for (int i = 0; i < n; i++)
        bytes += strlen(argv[i]) + 1;
    char **names = (char **)heap_alloc(HEAP_FILES, bytes);
    if (!names)
        return NULL;
    char *p = (char *)(names + n + 1);
    for (int i = 0; i < n; i++) {
        size_t len = strlen(argv[i]) + 1;
        memcpy(p, argv[i], len);
        names[i] = p;
        p += len;
    }
    names[n] = NULL;
    return names;
}

static void start(shell_t *sh, const char *cmd, bool keep, bool force, int n, char **files) {
    size_t need = sizeof(gzjob_t) + GZ_HEAP;
    size_t free_now = heap_free();
    if (free_now < need + HEAP_KEEP) {
        shell_printf(sh, "%s: not enough memory: needs %u KB and keeps %u KB free; %u KB free\n",
                     cmd, (unsigned)(need / 1024), HEAP_KEEP / 1024, (unsigned)(free_now / 1024));
        return;
    }
    const char *other = bigjob_try_begin(cmd);
    if (other) {
        shell_printf(sh, BIGJOB_BUSY_FMT, other);
        return;
    }
    gzjob_t *j = heap_zalloc(HEAP_FILES, sizeof(*j));
    char **names = j ? copy_names(n, files) : NULL;
    if (!names) {
        heap_release(HEAP_FILES, j);
        bigjob_end();
        shell_printf(sh, "%s: out of memory\n", cmd);
        return;
    }
    j->cmd = cmd;
    j->zcat = !strcmp(cmd, "zcat");
    j->keep = keep;
    j->force = force;
    j->names = names;
    j->n = n;
    files_hold();
    shell_run_job(sh, gz_step, j);
}

// gunzip [-k] [-f] <file>...: options first, single or together (-kf); `--` ends them.
static void run_gunzip(shell_t *sh, int argc, char **argv) {
    bool keep = false, force = false, ok = true;
    int i = 1;
    for (; ok && i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for (const char *o = argv[i] + 1; ok && *o; o++) {
            if (*o == 'k')
                keep = true;
            else if (*o == 'f')
                force = true;
            else
                ok = false;
        }
    }
    for (int k = i; ok && k < argc; k++)
        ok = argv[k][0] != '\0';
    if (!ok || i == argc) {
        shell_printf(sh, "usage: gunzip [-k] [-f] <file>...\n");
        return;
    }
    start(sh, "gunzip", keep, force, argc - i, argv + i);
}

static void run_zcat(shell_t *sh, int argc, char **argv) {
    if (argc != 2 || !argv[1][0]) {
        shell_printf(sh, "usage: zcat <file>\n");
        return;
    }
    start(sh, "zcat", false, false, 1, argv + 1);
}

const shell_cmd_t gzip_commands[] = {
    {"gunzip", NULL, run_gunzip, NULL, SHELL_PATH},
    {"zcat", NULL, run_zcat, NULL, SHELL_PATH},
    {0},
};

void gzip_help(shell_t *sh) {
    shell_printf(sh, "gunzip [-k] [-f] <file>...  decompress x.gz to x (-k keeps x.gz)\n");
    shell_printf(sh, "zcat <file>       print a gzip file decompressed (Ctrl-C stops)\n");
}
