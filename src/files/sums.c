// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// sha1sum, sha256sum, sha512sum (docs/design/archives.md §4):
//   <cmd> <file>...                        print "<hex>  <name>" for each file
//   <cmd> -c [--ignore-missing] <list>     check the files a list names
//
// A shell job, like cat: 4 KB read and hashed at a time, for STEP_US per main-loop pass, one
// file at a time; Ctrl-C stops it. Hashing is wolfCrypt's, on core 0 (the main loop), except
// that sha256sum hashes a file on the RP2350's SHA-256 block when it's free (sha256hw.h): the
// block is taken when a file is opened and released when it's closed, which every end path
// goes through (close_file()). The text (names shown, the list's lines, which names a list may
// give) is sums_parse.c's.

#include "files/sums.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/hash.h>

#include "ff.h"

#include "core/heaptag.h"
#include "files/files.h"
#include "files/path.h"
#include "files/sdcard.h"
#include "files/sha256hw.h"
#include "files/sums_parse.h"

#define PATH_LEN    256         // FatFs long names are up to 255 characters
#define STEP_US     20000       // work per job step before letting the main loop run
#define CHUNK       4096        // read and hashed at a time
#define OUT_ROOM    1024        // output space a line waits for (or its length, if shorter)

typedef struct {
    const char *cmd;
    const char *tag;            // the BSD form's name
    enum wc_HashType type;
    size_t size;                // digest bytes
} alg_t;

static const alg_t algs[] = {
    {"sha1sum", "SHA1", WC_HASH_TYPE_SHA, 20},
    {"sha256sum", "SHA256", WC_HASH_TYPE_SHA256, 32},
    {"sha512sum", "SHA512", WC_HASH_TYPE_SHA512, 64},
};

typedef struct {
    const alg_t *alg;
    bool check;                 // -c
    bool ignore_missing;
    // The file being hashed.
    bool open;
    FIL f;
    bool hw;                    // hashed on the SHA-256 block, not with `hash`
    wc_HashAlg hash;
    const char *name;           // as given (an argument) or as listed (in lines.buf)
    // Printing: the arguments left, copied into names[] one after another.
    int left;
    const char *next;
    // Checking.
    FIL list;
    bool list_eof;
    const char *list_name;      // as given
    char dir[PATH_LEN];         // the list's directory: names are relative to it
    sums_lines_t lines;
    sums_line_t entry;
    unsigned ok, failed, missing, unreadable, unsafe, bad;
    unsigned ignored;           // missing, with --ignore-missing
    _Alignas(4) uint8_t buf[CHUNK];     // the block takes aligned data a word at a time
    char names[];
} sums_job_t;

// Write a name as sums_name_char() shows it.
static void put_name(shell_t *sh, const char *name) {
    const char *run = name;
    for (const char *p = name;; p++) {
        const char *s = *p ? sums_name_char(*p) : "";
        if (!s)
            continue;
        shell_write(sh, run, (size_t)(p - run));
        if (!*p)
            return;
        shell_write(sh, s, strlen(s));
        run = p + 1;
    }
}

// "[<cmd>: ]<name>: <text>\n", the name shown safely (escaped and marked with '\', as GNU does).
static void say(shell_t *sh, const char *cmd, const char *name, const char *text) {
    if (cmd)
        shell_printf(sh, "%s: ", cmd);
    if (sums_name_flags(name) & SUMS_NAME_ESCAPED)
        shell_write(sh, "\\", 1);
    put_name(sh, name);
    shell_printf(sh, ": %s\n", text);
}

// Room for a line of about `len` bytes (its "\n" becomes "\r\n").
static bool room_for(shell_t *sh, size_t len) {
    size_t want = len + 2 < OUT_ROOM ? len + 2 : OUT_ROOM;
    return shell_write_space(sh) >= (int)want;
}

static void close_file(sums_job_t *j) {
    if (!j->open)
        return;
    f_close(&j->f);
    if (j->hw)
        sha256hw_abort();               // released (a no-op after sha256hw_finish())
    else
        wc_HashFree(&j->hash, j->alg->type);
    j->hw = false;
    j->open = false;
}

// Open `path` for hashing. NULL on success, else why not (FatFs's FR_NO_FILE for a directory
// is said as such); *missing for no such file or directory.
static const char *open_file(sums_job_t *j, const char *path, bool *missing) {
    *missing = false;
    FRESULT fr = f_open(&j->f, path, FA_READ);
    if (fr == FR_NO_FILE) {
        FILINFO fi;
        if (!strcmp(path, "/") || (f_stat(path, &fi) == FR_OK && (fi.fattrib & AM_DIR)))
            return "is a directory";
    }
    if (fr != FR_OK) {
        *missing = fr == FR_NO_FILE || fr == FR_NO_PATH;
        return files_error(fr);
    }
    j->hw = j->alg->type == WC_HASH_TYPE_SHA256 && sha256hw_begin();
    if (!j->hw && wc_HashInit(&j->hash, j->alg->type) != 0) {
        f_close(&j->f);
        return "the hash couldn't start";
    }
    j->open = true;
    return NULL;
}

typedef enum { HASH_MORE, HASH_DONE, HASH_ERROR } hash_step_t;

// Hash the open file until it ends or STEP_US has passed since t0. On HASH_ERROR, *why says
// why and the file is closed.
static hash_step_t hash_some(sums_job_t *j, uint64_t t0, const char **why) {
    while (time_us_64() - t0 < STEP_US) {
        UINT br;
        FRESULT fr = f_read(&j->f, j->buf, sizeof(j->buf), &br);
        if (fr != FR_OK) {
            *why = files_error(fr);
            close_file(j);
            return HASH_ERROR;
        }
        if (br == 0)
            return HASH_DONE;
        if (j->hw)
            sha256hw_update(j->buf, br);
        else if (wc_HashUpdate(&j->hash, j->alg->type, j->buf, br) != 0) {
            *why = "the hash failed";
            close_file(j);
            return HASH_ERROR;
        }
    }
    return HASH_MORE;
}

// The open file's digest into out; the file is closed. False if the hash failed.
static bool finish_file(sums_job_t *j, uint8_t *out) {
    bool ok = true;
    if (j->hw)
        sha256hw_finish(out);
    else
        ok = wc_HashFinal(&j->hash, j->alg->type, out) == 0;
    close_file(j);
    return ok;
}

static bool job_end(sums_job_t *j) {
    close_file(j);
    if (j->check)
        f_close(&j->list);
    heap_release(HEAP_FILES, j);
    files_release();
    return false;
}

// --- printing ------------------------------------------------------------------------------------

// Start on the next argument; false when there is none left.
static bool print_next(shell_t *sh, sums_job_t *j) {
    while (j->left > 0) {
        const char *name = j->next;
        j->next += strlen(name) + 1;
        j->left--;
        j->name = name;
        char path[PATH_LEN];
        bool missing;
        const char *why;
        if (sums_name_flags(name) & SUMS_NAME_CONTROL)
            why = "name has control characters";
        else if (!path_resolve(sh->cwd, name, path, sizeof(path)))
            why = "path too long";
        else
            why = open_file(j, path, &missing);
        if (!why)
            return true;
        say(sh, j->alg->cmd, name, why);
    }
    return false;
}

static bool print_step(shell_t *sh, sums_job_t *j) {
    uint64_t t0 = time_us_64();
    while (time_us_64() - t0 < STEP_US) {
        if (!j->open && !room_for(sh, OUT_ROOM))
            return true;                // for what the next name may say
        if (!j->open && !print_next(sh, j))
            return false;
        if (!room_for(sh, OUT_ROOM))
            return true;                // a read error closes the file and says a line at once
        const char *why = NULL;
        hash_step_t r = hash_some(j, t0, &why);
        if (r == HASH_MORE)
            return true;
        if (r == HASH_ERROR) {
            say(sh, j->alg->cmd, j->name, why);
            continue;
        }
        if (!room_for(sh, 2 * j->alg->size + 3 + sums_name_len(j->name)))
            return true;                // wait for the client to read; the file stays open
        uint8_t digest[SUMS_DIGEST_MAX];
        if (!finish_file(j, digest)) {
            say(sh, j->alg->cmd, j->name, "the hash failed");
            continue;
        }
        char hex[2 * SUMS_DIGEST_MAX + 1];
        for (size_t i = 0; i < j->alg->size; i++)
            snprintf(hex + 2 * i, 3, "%02x", digest[i]);
        bool escaped = sums_name_flags(j->name) & SUMS_NAME_ESCAPED;
        shell_printf(sh, "%s%s  ", escaped ? "\\" : "", hex);
        put_name(sh, j->name);
        shell_write(sh, "\n", 1);
    }
    return true;
}

// --- checking ------------------------------------------------------------------------------------

typedef enum {
    NEXT_OPENED,                // a listed file is open, to hash
    NEXT_SAID,                  // a line was said and counted: call again (after room_for())
    NEXT_END,                   // the list ended, or couldn't be read (said)
} next_t;

// Go on to the next line of the list that names a file, and open the file. A line that isn't
// checked is said and counted here.
static next_t check_next(shell_t *sh, sums_job_t *j) {
    for (;;) {
        char *line;
        size_t len;
        sums_next_t n = sums_lines_next(&j->lines, j->list_eof, &line, &len);
        if (n == SUMS_NEXT_END)
            return NEXT_END;
        if (n == SUMS_NEXT_MORE) {
            size_t room;
            char *to = sums_lines_room(&j->lines, &room);
            UINT br;
            FRESULT fr = f_read(&j->list, to, (UINT)room, &br);
            if (fr != FR_OK) {
                say(sh, j->alg->cmd, j->list_name, files_error(fr));
                return NEXT_END;
            }
            j->lines.len += br;
            j->list_eof = br == 0;
            continue;
        }
        if (n == SUMS_NEXT_LONG) {
            shell_printf(sh, "line %u: too long\n", j->lines.line);
            j->bad++;
            return NEXT_SAID;
        }
        sums_line_kind_t k = sums_parse_line(line, len, j->alg->tag, j->alg->size, &j->entry);
        if (k == SUMS_LINE_SKIP)
            continue;
        if (k == SUMS_LINE_BAD) {
            j->bad++;
            continue;
        }
        const char *name = j->entry.name;
        j->name = name;
        char path[PATH_LEN];
        if (!sums_name_safe(name)) {
            say(sh, NULL, name, "unsafe path");
            j->unsafe++;
            return NEXT_SAID;
        }
        if (!path_resolve(j->dir, name, path, sizeof(path))) {
            say(sh, NULL, name, "path too long");
            j->unreadable++;
            return NEXT_SAID;
        }
        bool missing;
        const char *why = open_file(j, path, &missing);
        if (!why)
            return NEXT_OPENED;
        if (missing && j->ignore_missing) {
            j->ignored++;               // not counted in the summary
            continue;
        }
        say(sh, NULL, name, missing ? "missing" : why);
        if (missing)
            j->missing++;
        else
            j->unreadable++;
        return NEXT_SAID;
    }
}

static void check_summary(shell_t *sh, sums_job_t *j) {
    if (!j->ok && !j->failed && !j->missing && !j->unreadable && !j->unsafe) {
        say(sh, j->alg->cmd, j->list_name, j->ignored ? "no file was verified (all missing)"
                                                      : "no checksums found");
        return;
    }
    char line[160];
    int n = snprintf(line, sizeof(line), "%u OK", j->ok);
    const struct { unsigned count; const char *what; } parts[] = {
        {j->failed, "FAILED"}, {j->missing, "missing"}, {j->unreadable, "unreadable"},
        {j->unsafe, "unsafe"}, {j->bad, "improperly formatted"},
    };
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
        if (parts[i].count && n > 0 && (size_t)n < sizeof(line))
            n += snprintf(line + n, sizeof(line) - (size_t)n, ", %u %s", parts[i].count,
                          parts[i].what);
    shell_printf(sh, "%s\n", line);
}

static bool check_step(shell_t *sh, sums_job_t *j) {
    uint64_t t0 = time_us_64();
    while (time_us_64() - t0 < STEP_US) {
        if (!j->open) {
            if (!room_for(sh, OUT_ROOM))    // for what the next line may say
                return true;
            next_t n = check_next(sh, j);
            if (n == NEXT_END) {
                check_summary(sh, j);
                return false;
            }
            if (n == NEXT_SAID)
                continue;
        }
        if (!room_for(sh, OUT_ROOM))
            return true;                // a read error closes the file and says a line at once
        const char *why = NULL;
        hash_step_t r = hash_some(j, t0, &why);
        if (r == HASH_MORE)
            return true;
        if (r == HASH_ERROR) {
            say(sh, NULL, j->name, why);
            j->unreadable++;
            continue;
        }
        if (!room_for(sh, sums_name_len(j->name) + 16))
            return true;
        uint8_t digest[SUMS_DIGEST_MAX];
        if (!finish_file(j, digest)) {
            say(sh, NULL, j->name, "the hash failed");
            j->unreadable++;
        } else if (memcmp(digest, j->entry.digest, j->alg->size)) {
            say(sh, NULL, j->name, "FAILED");
            j->failed++;
        } else {
            say(sh, NULL, j->name, "OK");
            j->ok++;
        }
    }
    return true;
}

static bool sums_step(shell_t *sh, void *arg, bool cancel) {
    sums_job_t *j = arg;
    if (cancel) {
        if (j->check)
            shell_printf(sh, "%s: stopped\n", j->alg->cmd);
        return job_end(j);
    }
    bool more = j->check ? check_step(sh, j) : print_step(sh, j);
    return more ? true : job_end(j);
}

// --- the command ---------------------------------------------------------------------------------

static void usage(shell_t *sh, const alg_t *alg) {
    shell_printf(sh, "usage: %s <file>...\n       %s -c [--ignore-missing] <list>\n", alg->cmd,
                 alg->cmd);
}

static void sums_command(shell_t *sh, int argc, char **argv) {
    const alg_t *alg = &algs[0];
    for (size_t i = 0; i < sizeof(algs) / sizeof(algs[0]); i++)
        if (!strcmp(argv[0], algs[i].cmd))
            alg = &algs[i];
    bool check = false, ignore_missing = false;
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; first++) {
        if (!strcmp(argv[first], "--")) {
            first++;
            break;
        }
        if (!strcmp(argv[first], "-c")) {
            check = true;
        } else if (!strcmp(argv[first], "--ignore-missing")) {
            ignore_missing = true;
        } else {
            shell_printf(sh, "%s: unknown option %s\n", alg->cmd, argv[first]);
            usage(sh, alg);
            return;
        }
    }
    int count = argc - first;
    bool ok = count >= 1 && (!check || count == 1) && (check || !ignore_missing);
    size_t bytes = 0;
    for (int i = first; ok && i < argc; i++) {
        ok = argv[i][0] != '\0';
        bytes += strlen(argv[i]) + 1;
    }
    if (!ok) {
        if (ignore_missing && !check)
            shell_printf(sh, "%s: --ignore-missing is for -c\n", alg->cmd);
        usage(sh, alg);
        return;
    }
    if (!sdcard_mount(sh))
        return;
    sums_job_t *j = heap_zalloc(HEAP_FILES, sizeof(*j) + bytes);
    if (!j) {
        shell_printf(sh, "%s: out of memory\n", alg->cmd);
        return;
    }
    j->alg = alg;
    j->check = check;
    j->ignore_missing = ignore_missing;
    char *w = j->names;                 // the words outlive the line they came in
    for (int i = first; i < argc; i++) {
        size_t n = strlen(argv[i]) + 1;
        memcpy(w, argv[i], n);
        w += n;
    }
    j->next = j->names;
    j->left = count;
    if (check) {
        char path[PATH_LEN];
        j->list_name = j->names;
        if (!path_resolve(sh->cwd, j->list_name, path, sizeof(path))) {
            shell_printf(sh, "path too long\n");
            heap_release(HEAP_FILES, j);
            return;
        }
        FRESULT fr = f_open(&j->list, path, FA_READ);
        if (fr != FR_OK) {
            say(sh, alg->cmd, j->list_name, files_error(fr));
            heap_release(HEAP_FILES, j);
            return;
        }
        // The list's directory: the path without its last component.
        strcpy(j->dir, path);
        char *slash = strrchr(j->dir, '/');
        if (slash == j->dir)
            slash++;                    // in the root: keep "/"
        *slash = '\0';
    }
    files_hold();
    shell_run_job(sh, sums_step, j);
}

const shell_cmd_t sums_commands[] = {
    {"sha1sum", NULL, sums_command, NULL, SHELL_PATH},
    {"sha256sum", NULL, sums_command, NULL, SHELL_PATH},
    {"sha512sum", NULL, sums_command, NULL, SHELL_PATH},
    {0},
};
