// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// File commands on the SD card: ls, cd, pwd, cat, cp, mv, rm, mkdir, echo.
//
// Anything that can take long (listing a big directory, printing or copying a file) runs as a
// shell job: a bounded amount of work per main-loop pass, so SSH sessions, the console and the
// watchdog keep going, and Ctrl-C cancels. Output is sized by the channel's free space, so a slow
// SSH client slows the command down instead of losing output. Each shell has its own working
// directory. FatFs (FF_FS_LOCK) refuses to open, delete or rename a file another job has open.

#include "files/files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"

#include "ff.h"
#include "f_util.h"

#include "core/heaptag.h"
#include "files/path.h"
#include "files/sdcard.h"
#include "shell/complete.h"

#define PATH_LEN     256        // FatFs long names are up to 255 characters
#define STEP_US      20000      // work per job step before letting the main loop run
#define COPY_CHUNK   4096
#define LS_LINE_MAX  (PATH_LEN + 48)
#define TAB_DIR_MAX  256        // directory entries Tab reads at most (it blocks the main loop)

static int holders;             // jobs and SFTP handles with open files or directories

bool files_busy(void) {
    return holders > 0;
}

void files_hold(void) {
    holders++;
}

void files_release(void) {
    if (holders > 0)
        holders--;
}

// Protected paths (path_is_protected()): /certs holds the TLS trust anchors, /var/log the log
// files. Only the console (physical access, the same boundary as the configuration) may change
// them. SSH shells, SFTP and Lua (sh NULL) may read them but not change them: a session that
// gets in can't erase its own traces from the log. For /certs, TLS also checks the contents
// against a hash approved on the console, so this is a second line.
bool files_write_allowed(const shell_t *sh, const char *path) {
    if (!path_is_protected(path))
        return true;
    return sh && sh->io && sh->io->admin;
}

// For the commands: say so when a write is refused.
static bool may_write(shell_t *sh, const char *cmd, const char *path) {
    if (files_write_allowed(sh, path))
        return true;
    shell_printf(sh, "%s: %s: not allowed to change this path\n", cmd, path);
    return false;
}

// --- helpers ---------------------------------------------------------------------------------

const char *files_error(int fr) {
    switch ((FRESULT)fr) {
    case FR_NO_FILE:      return "no such file";
    case FR_NO_PATH:      return "no such directory";
    case FR_EXIST:        return "already exists";
    case FR_DENIED:       return "denied (directory not empty, read-only, or the card is full)";
    case FR_LOCKED:       return "in use by another command";
    case FR_INVALID_NAME: return "invalid name";
    case FR_NOT_READY:    return "card not ready";
    default:              return FRESULT_str(fr);
    }
}

// Resolve an argument against the shell's working directory and make sure the card is mounted.
static bool resolve(shell_t *sh, const char *arg, char *out) {
    if (!path_resolve(sh->cwd, arg ? arg : "", out, PATH_LEN)) {
        shell_printf(sh, "path too long\n");
        return false;
    }
    return sdcard_mount(sh);
}

static bool is_dir(const char *path) {
    if (!strcmp(path, "/"))
        return true;                // f_stat can't stat the root
    FILINFO fi;
    return f_stat(path, &fi) == FR_OK && (fi.fattrib & AM_DIR);
}

// cp/mv to a directory: the destination is <dir>/<basename of the source>.
static bool into_dir(shell_t *sh, const char *src, char *dst) {
    if (!is_dir(dst))
        return true;
    char dir[PATH_LEN];
    strcpy(dir, dst);
    if (!path_join(dir, path_basename(src), dst, PATH_LEN)) {
        shell_printf(sh, "path too long\n");
        return false;
    }
    return true;
}

// Split "a b" into two arguments (names with spaces aren't supported yet).
static bool two_args(shell_t *sh, char *args, char **a, char **b, const char *usage) {
    *a = args ? strtok(args, " ") : NULL;
    *b = *a ? strtok(NULL, " ") : NULL;
    if (!*a || !*b || strtok(NULL, " ")) {
        shell_printf(sh, "usage: %s\n", usage);
        return false;
    }
    return true;
}

static void job_start(shell_t *sh, shell_job_fn step, void *arg) {
    files_hold();
    shell_run_job(sh, step, arg);
}

static bool job_done(void *arg) {
    heap_release(HEAP_FILES, arg);
    files_release();
    return false;
}

// Names come from the card: keep control characters (escape sequences) off the terminal.
static const char *printable(char *name) {
    for (char *p = name; *p; p++)
        if ((unsigned char)*p < 0x20 || *p == 0x7f)
            *p = '?';
    return name;
}

// --- ls ----------------------------------------------------------------------------------------

typedef struct {
    DIR dir;
    unsigned files, dirs;
    uint64_t bytes;
} ls_job_t;

static bool ls_step(shell_t *sh, void *arg, bool cancel) {
    ls_job_t *j = arg;
    bool finished = cancel;
    for (int i = 0; !finished && i < 16; i++) {     // up to 16 entries per pass
        if (shell_write_space(sh) < LS_LINE_MAX)
            return true;                            // wait for the client to read
        FILINFO fi;
        FRESULT fr = f_readdir(&j->dir, &fi);
        if (fr != FR_OK || !fi.fname[0]) {
            if (fr != FR_OK)
                shell_printf(sh, "ls: %s\n", files_error(fr));
            shell_printf(sh, "%u files, %u directories, %llu bytes\n", j->files, j->dirs,
                         (unsigned long long)j->bytes);
            finished = true;
            break;
        }
        char date[20];
        snprintf(date, sizeof(date), "%04u-%02u-%02u %02u:%02u", (fi.fdate >> 9) + 1980,
                 (fi.fdate >> 5) & 15, fi.fdate & 31, fi.ftime >> 11, (fi.ftime >> 5) & 63);
        if (fi.fattrib & AM_DIR) {
            shell_printf(sh, "%12s  %s  %s/\n", "<dir>", date, printable(fi.fname));
            j->dirs++;
        } else {
            shell_printf(sh, "%12llu  %s  %s\n", (unsigned long long)fi.fsize, date,
                         printable(fi.fname));
            j->files++;
            j->bytes += fi.fsize;
        }
    }
    if (!finished)
        return true;
    f_closedir(&j->dir);
    return job_done(j);
}

static void cmd_ls(shell_t *sh, const char *arg) {
    char path[PATH_LEN];
    if (!resolve(sh, arg, path))
        return;
    if (!is_dir(path)) {
        FILINFO fi;
        FRESULT fr = f_stat(path, &fi);
        if (fr != FR_OK)
            shell_printf(sh, "ls: %s: %s\n", arg, files_error(fr));
        else
            shell_printf(sh, "%12llu  %s\n", (unsigned long long)fi.fsize, printable(fi.fname));
        return;
    }
    ls_job_t *j = heap_zalloc(HEAP_FILES, sizeof(*j));
    if (!j) {
        shell_printf(sh, "ls: out of memory\n");
        return;
    }
    FRESULT fr = f_opendir(&j->dir, path);
    if (fr != FR_OK) {
        shell_printf(sh, "ls: %s\n", files_error(fr));
        heap_release(HEAP_FILES, j);
        return;
    }
    job_start(sh, ls_step, j);
}

// --- cd / pwd / mkdir / rm / mv / echo ---------------------------------------------------------

static void cmd_cd(shell_t *sh, const char *arg) {
    char path[PATH_LEN];
    if (!resolve(sh, arg ? arg : "/", path))
        return;
    if (strlen(path) >= sizeof(sh->cwd)) {
        shell_printf(sh, "cd: path too long (%u characters at most)\n",
                     (unsigned)sizeof(sh->cwd) - 1);
        return;
    }
    if (!is_dir(path)) {
        shell_printf(sh, "cd: %s: no such directory\n", arg);
        return;
    }
    strcpy(sh->cwd, path);
}

static void cmd_mkdir(shell_t *sh, const char *arg) {
    char path[PATH_LEN];
    if (!arg) {
        shell_printf(sh, "usage: mkdir <dir>\n");
        return;
    }
    if (!resolve(sh, arg, path) || !may_write(sh, "mkdir", path))
        return;
    FRESULT fr = f_mkdir(path);
    if (fr != FR_OK)
        shell_printf(sh, "mkdir: %s: %s\n", arg, files_error(fr));
}

static void cmd_rm(shell_t *sh, const char *arg) {
    char path[PATH_LEN];
    if (!arg) {
        shell_printf(sh, "usage: rm <file or empty directory>\n");
        return;
    }
    if (!resolve(sh, arg, path))
        return;
    size_t len = strlen(path);
    if (!strcmp(path, "/") ||
        (!strncmp(sh->cwd, path, len) && (sh->cwd[len] == '\0' || sh->cwd[len] == '/'))) {
        shell_printf(sh, "rm: %s: is the current directory or above it\n", arg);
        return;
    }
    if (!may_write(sh, "rm", path))
        return;
    FRESULT fr = f_unlink(path);
    if (fr != FR_OK)
        shell_printf(sh, "rm: %s: %s\n", arg, files_error(fr));
}

static void cmd_mv(shell_t *sh, char *args) {
    char *a, *b, src[PATH_LEN], dst[PATH_LEN];
    if (!two_args(sh, args, &a, &b, "mv <from> <to>"))
        return;
    if (!resolve(sh, a, src) || !resolve(sh, b, dst) || !into_dir(sh, src, dst) ||
        !may_write(sh, "mv", src) || !may_write(sh, "mv", dst))
        return;
    FRESULT fr = f_rename(src, dst);
    if (fr != FR_OK)
        shell_printf(sh, "mv: %s\n", files_error(fr));
}

// echo <text> [> file | >> file]
static void cmd_echo(shell_t *sh, char *args) {
    if (!args) {
        shell_printf(sh, "\n");
        return;
    }
    char *text = args;
    char *redir = strrchr(text, '>');
    if (!redir) {
        shell_printf(sh, "%s\n", text);
        return;
    }
    bool append = redir > text && redir[-1] == '>';
    char *name = redir + 1;
    char *end = append ? redir - 1 : redir;
    while (*name == ' ')
        name++;
    while (end > text && end[-1] == ' ')
        end--;
    *end = '\0';
    char path[PATH_LEN];
    if (!*name) {
        shell_printf(sh, "usage: echo <text> [> file | >> file]\n");
        return;
    }
    if (!resolve(sh, name, path) || !may_write(sh, "echo", path))
        return;
    FIL f;
    FRESULT fr = f_open(&f, path, FA_WRITE | (append ? FA_OPEN_APPEND : FA_CREATE_ALWAYS));
    if (fr == FR_OK) {
        UINT bw;
        size_t n = strlen(text);
        text[n] = '\n';
        fr = f_write(&f, text, (UINT)n + 1, &bw);
        text[n] = '\0';
        if (fr == FR_OK && bw != n + 1)
            fr = FR_DENIED;         // card full
        FRESULT cr = f_close(&f);
        if (fr == FR_OK)
            fr = cr;
    }
    if (fr != FR_OK)
        shell_printf(sh, "echo: %s: %s\n", name, files_error(fr));
}

// --- cat ---------------------------------------------------------------------------------------

typedef struct {
    FIL f;
    char buf[512];
} cat_job_t;

static bool cat_step(shell_t *sh, void *arg, bool cancel) {
    cat_job_t *j = arg;
    bool finished = cancel;
    uint64_t t0 = time_us_64();
    while (!finished && time_us_64() - t0 < STEP_US) {
        int space = shell_write_space(sh) / 2;      // room for "\n" -> "\r\n"
        if (space < 64)
            return true;                            // wait for the client to read
        UINT n = space < (int)sizeof(j->buf) ? (UINT)space : sizeof(j->buf);
        UINT br;
        FRESULT fr = f_read(&j->f, j->buf, n, &br);
        if (fr != FR_OK)
            shell_printf(sh, "\ncat: %s\n", files_error(fr));
        if (fr != FR_OK || br == 0)
            finished = true;                        // error or end of file
        else
            shell_write(sh, j->buf, br);
    }
    if (!finished)
        return true;
    f_close(&j->f);
    return job_done(j);
}

static void cmd_cat(shell_t *sh, const char *arg) {
    char path[PATH_LEN];
    if (!arg) {
        shell_printf(sh, "usage: cat <file>\n");
        return;
    }
    if (!resolve(sh, arg, path))
        return;
    cat_job_t *j = heap_alloc(HEAP_FILES, sizeof(*j));
    if (!j) {
        shell_printf(sh, "cat: out of memory\n");
        return;
    }
    FRESULT fr = f_open(&j->f, path, FA_READ);
    if (fr != FR_OK) {
        shell_printf(sh, "cat: %s: %s\n", arg, files_error(fr));
        heap_release(HEAP_FILES, j);
        return;
    }
    job_start(sh, cat_step, j);
}

// --- cp ----------------------------------------------------------------------------------------

typedef struct {
    FIL src, dst;
    char dst_path[PATH_LEN];
    uint64_t done, size, t0;
    uint8_t buf[COPY_CHUNK];
} cp_job_t;

static bool cp_step(shell_t *sh, void *arg, bool cancel) {
    cp_job_t *j = arg;
    FRESULT fr = FR_OK;
    bool finished = false;
    uint64_t t0 = time_us_64();
    while (!cancel && time_us_64() - t0 < STEP_US) {
        UINT br, bw;
        fr = f_read(&j->src, j->buf, sizeof(j->buf), &br);
        if (fr != FR_OK)
            break;
        if (br == 0) {
            finished = true;
            break;
        }
        fr = f_write(&j->dst, j->buf, br, &bw);
        if (fr == FR_OK && bw != br)
            fr = FR_DENIED;                         // card full
        if (fr != FR_OK)
            break;
        j->done += br;
    }
    if (!cancel && !finished && fr == FR_OK)
        return true;

    f_close(&j->src);
    FRESULT cr = f_close(&j->dst);
    if (fr == FR_OK)
        fr = cr;
    if (cancel || fr != FR_OK) {
        f_unlink(j->dst_path);                      // don't leave a partial copy
        if (!cancel)
            shell_printf(sh, "cp: %s\n", files_error(fr));
        shell_printf(sh, "cp: stopped after %llu of %llu bytes; partial copy removed\n",
                     (unsigned long long)j->done, (unsigned long long)j->size);
    } else {
        uint64_t us = time_us_64() - j->t0;
        shell_printf(sh, "copied %llu bytes in %llu ms (%llu KB/s)\n", (unsigned long long)j->done,
                     (unsigned long long)(us / 1000),
                     (unsigned long long)(us ? j->done * 1000000 / 1024 / us : 0));
    }
    return job_done(j);
}

static void cmd_cp(shell_t *sh, char *args) {
    char *a, *b, src[PATH_LEN];
    if (!two_args(sh, args, &a, &b, "cp <from> <to>"))
        return;
    cp_job_t *j = heap_alloc(HEAP_FILES, sizeof(*j));
    if (!j) {
        shell_printf(sh, "cp: out of memory\n");
        return;
    }
    if (!resolve(sh, a, src) || !resolve(sh, b, j->dst_path) || !into_dir(sh, src, j->dst_path))
        goto fail;
    if (!may_write(sh, "cp", j->dst_path))
        goto fail;
    if (!strcmp(src, j->dst_path)) {
        shell_printf(sh, "cp: source and destination are the same\n");
        goto fail;
    }
    FRESULT fr = f_open(&j->src, src, FA_READ);
    if (fr != FR_OK) {
        shell_printf(sh, "cp: %s: %s\n", a, files_error(fr));
        goto fail;
    }
    fr = f_open(&j->dst, j->dst_path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        shell_printf(sh, "cp: %s: %s\n", b, files_error(fr));
        f_close(&j->src);
        goto fail;
    }
    j->done = 0;
    j->size = f_size(&j->src);
    j->t0 = time_us_64();
    job_start(sh, cp_step, j);
    return;
fail:
    heap_release(HEAP_FILES, j);
}

// --- dispatch ----------------------------------------------------------------------------------

// --- Tab completion ------------------------------------------------------------------------------

// The SD card's lister for Tab (shell/complete.h). Only while the card is mounted: a Tab
// mustn't wait for a card to start up, nor log a failure each time there's none. At most
// TAB_DIR_MAX entries, about 20 sectors of a directory read with the main loop waiting.
complete_dir_t shell_list_dir(shell_t *sh, const char *dir, complete_entry_fn fn, void *arg) {
    char path[PATH_LEN];
    if (!sdcard_mounted() || !path_resolve(sh->cwd, dir, path, sizeof(path)))
        return COMPLETE_DIR_NONE;
    DIR d;
    if (f_opendir(&d, path) != FR_OK)
        return COMPLETE_DIR_NONE;
    complete_dir_t r = COMPLETE_DIR_ALL;
    FILINFO fi;
    for (unsigned n = 0;; n++) {
        if (f_readdir(&d, &fi) != FR_OK || !fi.fname[0])
            break;
        if (n == TAB_DIR_MAX) {
            r = COMPLETE_DIR_PART;
            break;
        }
        if (strcmp(fi.fname, ".") && strcmp(fi.fname, ".."))
            fn(fi.fname, fi.fattrib & AM_DIR, arg);
    }
    f_closedir(&d);
    return r;
}

static void run_ls(shell_t *sh, char *args) { cmd_ls(sh, args); }
static void run_cd(shell_t *sh, char *args) { cmd_cd(sh, args); }
static void run_pwd(shell_t *sh, char *args) { (void)args; shell_printf(sh, "%s\n", sh->cwd); }
static void run_cat(shell_t *sh, char *args) { cmd_cat(sh, args); }
static void run_rm(shell_t *sh, char *args) { cmd_rm(sh, args); }
static void run_mkdir(shell_t *sh, char *args) { cmd_mkdir(sh, args); }

const shell_cmd_t files_commands[] = {
    {"ls", run_ls, NULL, SHELL_PATH},
    {"cd", run_cd, NULL, SHELL_PATH | SHELL_DIR},
    {"pwd", run_pwd, NULL, 0},
    {"cat", run_cat, NULL, SHELL_PATH},
    {"cp", cmd_cp, NULL, SHELL_PATH},
    {"mv", cmd_mv, NULL, SHELL_PATH},
    {"rm", run_rm, NULL, SHELL_PATH},
    {"mkdir", run_mkdir, NULL, SHELL_PATH | SHELL_DIR},
    {"echo", cmd_echo, NULL, SHELL_REDIRECT},
    {0},
};

void files_help(shell_t *sh) {
    shell_printf(sh, "ls [path]         list a directory on the SD card\n");
    shell_printf(sh, "cd [dir], pwd     change / show the working directory\n");
    shell_printf(sh, "cat <file>        print a file (Ctrl-C stops)\n");
    shell_printf(sh, "cp <from> <to>    copy a file (Ctrl-C stops)\n");
    shell_printf(sh, "mv <from> <to>    move or rename\n");
    shell_printf(sh, "rm <path>         delete a file or an empty directory\n");
    shell_printf(sh, "mkdir <dir>       create a directory\n");
    shell_printf(sh, "echo <text> [> file | >> file]  print, or write/append a line\n");
}
