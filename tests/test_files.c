// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/files/files.c: the file commands' arguments, run from typed lines the way
// the shell runs them (words_split(), then shell.c's dispatch), over a card in memory that
// stands in for FatFs (tests/stubs/ff.h). And the command tables' rule (shell_tables_check(),
// src/shell/cmdcheck.c) over files_commands and over tables made to break it. ls's listing, and
// when a file that tee, `>` or cp is writing gets synced (#218), with the real src/shell/pipe.c
// for the redirect.

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

#include "ff.h"
#include "f_util.h"
#include "pico/stdlib.h"

#include "files/files.h"
#include "files/sdcard.h"
#include "shell/complete.h"
#include "shell/shell.h"
#include "shell/words.h"
#include "test.h"

// --- a card in memory, behind FatFs's functions -------------------------------------------------

#define CARD_FILES 16

// FatFs writes a file's directory entry (size, date) only at f_sync() and f_close(): until then
// f_stat() and f_readdir() see the size it had, and a new file shows date and time zero.
#define DATE_OLD  ((46u << 9) | (11u << 5) | 27u)     // 2026-11-27, a file already on the card
#define TIME_OLD  ((17u << 11) | (42u << 5))          // 17:42
#define DATE_NOW  ((46u << 9) | (11u << 5) | 28u)     // 2026-11-28, written out by sync or close
#define TIME_NOW  ((9u << 11) | (5u << 5))            // 09:05

static struct {
    char path[256];             // absolute; "" for a free slot
    bool dir;
    char data[256];
    size_t len;
    size_t entry_len;           // the size in the directory entry
    uint16_t fdate, ftime;      // the date in the directory entry
} card[CARD_FILES];

#define SYNCS_MAX 16
typedef struct {
    int slot;
    size_t len;                 // the file's length then
    uint64_t at;                // the clock then
} sync_t;
static sync_t syncs[SYNCS_MAX];
static int nsyncs;
static FRESULT sync_result;     // what f_sync() returns
static UINT read_max;           // f_read() reads at most this much (0: no limit)

static void card_clear(void) {
    memset(card, 0, sizeof(card));
}

static int card_find(const char *path) {
    for (int i = 0; i < CARD_FILES; i++)
        if (card[i].path[0] && !strcmp(card[i].path, path))
            return i;
    return -1;
}

static int card_add(const char *path, bool dir, const char *data) {
    for (int i = 0; i < CARD_FILES; i++)
        if (!card[i].path[0]) {
            snprintf(card[i].path, sizeof(card[i].path), "%s", path);
            card[i].dir = dir;
            snprintf(card[i].data, sizeof(card[i].data), "%s", data ? data : "");
            card[i].len = card[i].entry_len = strlen(card[i].data);
            card[i].fdate = DATE_OLD;
            card[i].ftime = TIME_OLD;
            return i;
        }
    return -1;
}

// The clock moves `tick` on each reading (cp's steps end on it), and the test moves it on.
static uint64_t now, tick = 1000;

uint64_t time_us_64(void) {
    return now += tick;
}

FSIZE_t test_f_size(const FIL *fp) {
    return card[fp->slot].len;
}

FRESULT f_open(FIL *fp, const char *path, uint8_t mode) {
    int i = card_find(path);
    if (i >= 0 && card[i].dir)
        return FR_DENIED;
    if (mode & FA_WRITE) {
        if (i < 0 && (i = card_add(path, false, "")) >= 0)
            card[i].fdate = card[i].ftime = 0;      // new: the entry is written at sync
        else if ((mode & FA_OPEN_APPEND) != FA_OPEN_APPEND)
            card[i].len = 0;
        if (i < 0)
            return FR_DENIED;
    } else if (i < 0) {
        return FR_NO_FILE;
    }
    fp->slot = i;
    fp->mode = mode;
    fp->pos = (mode & FA_OPEN_APPEND) == FA_OPEN_APPEND ? card[i].len : 0;
    return FR_OK;
}

static void entry_write(int i) {
    card[i].entry_len = card[i].len;
    card[i].fdate = DATE_NOW;
    card[i].ftime = TIME_NOW;
}

FRESULT f_sync(FIL *fp) {
    if (nsyncs < SYNCS_MAX)
        syncs[nsyncs++] = (sync_t){fp->slot, card[fp->slot].len, now};
    if (sync_result == FR_OK)
        entry_write(fp->slot);
    return sync_result;
}

FRESULT f_close(FIL *fp) {
    if (fp->mode & FA_WRITE && card[fp->slot].path[0])
        entry_write(fp->slot);
    return FR_OK;
}

FRESULT f_read(FIL *fp, void *buf, UINT btr, UINT *br) {
    size_t left = card[fp->slot].len - fp->pos;
    if (read_max && btr > read_max)
        btr = read_max;
    *br = btr < left ? btr : (UINT)left;
    memcpy(buf, card[fp->slot].data + fp->pos, *br);
    fp->pos += *br;
    return FR_OK;
}

FRESULT f_write(FIL *fp, const void *buf, UINT btw, UINT *bw) {
    size_t room = sizeof(card[0].data) - 1 - fp->pos;
    *bw = btw < room ? btw : (UINT)room;
    memcpy(card[fp->slot].data + fp->pos, buf, *bw);
    fp->pos += *bw;
    if (fp->pos > card[fp->slot].len)
        card[fp->slot].len = fp->pos;
    card[fp->slot].data[card[fp->slot].len] = '\0';
    return FR_OK;
}

FRESULT f_stat(const char *path, FILINFO *fno) {
    int i = card_find(path);
    if (i < 0)
        return FR_NO_FILE;
    memset(fno, 0, sizeof(*fno));
    fno->fsize = card[i].entry_len;
    fno->fdate = card[i].fdate;
    fno->ftime = card[i].ftime;
    fno->fattrib = card[i].dir ? AM_DIR : 0;
    const char *slash = strrchr(path, '/');
    snprintf(fno->fname, sizeof(fno->fname), "%s", slash ? slash + 1 : path);
    return FR_OK;
}

static char listing[256];       // the directory ls opened

FRESULT f_opendir(DIR *dp, const char *path) {
    if (strcmp(path, "/") && (card_find(path) < 0 || !card[card_find(path)].dir))
        return FR_NO_PATH;
    snprintf(listing, sizeof(listing), "%s", path);
    dp->next = 0;
    return FR_OK;
}

// The entries directly in `listing`, in the card's order.
FRESULT f_readdir(DIR *dp, FILINFO *fno) {
    size_t n = strcmp(listing, "/") ? strlen(listing) : 0;
    for (; dp->next < CARD_FILES; dp->next++) {
        const char *p = card[dp->next].path;
        if (p[0] && !strncmp(p, listing, n) && p[n] == '/' && !strchr(p + n + 1, '/'))
            return f_stat(card[dp->next++].path, fno);
    }
    fno->fname[0] = '\0';
    return FR_OK;
}

FRESULT f_closedir(DIR *dp) {
    (void)dp;
    return FR_OK;
}

FRESULT f_mkdir(const char *path) {
    if (card_find(path) >= 0)
        return FR_EXIST;
    return card_add(path, true, NULL) >= 0 ? FR_OK : FR_DENIED;
}

FRESULT f_unlink(const char *path) {
    int i = card_find(path);
    if (i < 0)
        return FR_NO_FILE;
    card[i].path[0] = '\0';
    return FR_OK;
}

FRESULT f_rename(const char *path_old, const char *path_new) {
    int i = card_find(path_old);
    if (i < 0)
        return FR_NO_FILE;
    if (card_find(path_new) >= 0)
        return FR_EXIST;
    snprintf(card[i].path, sizeof(card[i].path), "%s", path_new);
    return FR_OK;
}

const char *FRESULT_str(FRESULT i) {
    (void)i;
    return "error";
}

bool sdcard_mount(shell_t *sh) {
    (void)sh;
    return true;
}

bool sdcard_mounted(void) {
    return true;
}

// --- a shell: its output, and jobs run to the end at once ---------------------------------------

static char out[4096];
static size_t out_len;

static void term_out(shell_t *s, const char *buf, size_t len);

static bool producing;          // run() is running a line's command into its stages

// As shell_core.c: the line's command writes into its pipeline.
void shell_write(shell_t *s, const char *buf, size_t len) {
    if (producing && s->pipe.first)
        pipe_write(&s->pipe, buf, len);
    else
        term_out(s, buf, len);
}

static void term_out(shell_t *s, const char *buf, size_t len) {
    (void)s;
    if (out_len + len >= sizeof(out))
        len = sizeof(out) - 1 - out_len;
    memcpy(out + out_len, buf, len);
    out_len += len;
    out[out_len] = '\0';
}

void shell_printf(shell_t *sh, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
        shell_write(sh, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}

int shell_write_space(shell_t *sh) {
    (void)sh;
    return 1024;
}

void shell_run_job(shell_t *sh, shell_job_fn step, void *arg) {
    while (step(sh, arg, false)) {
    }
}

static const shell_io_t io_console = {.name = "console", .admin = true};
static shell_t sh;

static void out_clear(void) {
    out_len = 0;
    out[0] = '\0';
}

// The stages of `rest` (`> /f`, `| tee /f`) as shell_core.c opens them for a line, to be fed with
// pipe_write() as a long command's output would be, and ended with pipe_end().
static bool open_stages(const char *rest) {
    static char buf[SHELL_LINE_MAX + 1];
    words_t w;
    snprintf(buf, sizeof(buf), "%s", rest);
    out_clear();
    CHECK(words_split(buf, &w) == NULL);
    if (!pipe_open(&sh, &sh.pipe, &w, 0))
        return false;
    sh.pipe.out = term_out;
    return true;
}

// The line as the shell runs it: split into words, the stages after the command opened, then
// shell.c's shell_execute() over files_commands (runv gets the words, run their join), and the
// stages ended (the jobs here run to the end at once). Stages opened with open_stages() stay as
// they are: what the line's command prints goes to the terminal.
static void run(const char *line) {
    char buf[SHELL_LINE_MAX + 1];
    words_t w;
    snprintf(buf, sizeof(buf), "%s", line);
    out_clear();
    CHECK(words_split(buf, &w) == NULL);
    int argc = 0;
    while (argc < w.n && w.v[argc])
        argc++;
    bool staged = argc < w.n;
    if (staged) {
        if (!pipe_open(&sh, &sh.pipe, &w, argc))
            return;
        sh.pipe.out = term_out;
    }
    for (const shell_cmd_t *c = files_commands; c->name; c++)
        if (!strcmp(w.v[0], c->name)) {
            producing = staged;
            if (c->runv)
                c->runv(&sh, argc, w.v);
            else
                c->run(&sh, words_join(argc, w.v));
            producing = false;
            if (staged)
                pipe_end(&sh.pipe);
            return;
        }
    CHECK(!"no such command");
}

static void setup(void) {
    card_clear();
    nsyncs = 0;
    sync_result = FR_OK;
    read_max = 0;
    tick = 1000;
    memset(&sh, 0, sizeof(sh));
    sh.io = &io_console;
    strcpy(sh.cwd, "/");
    card_add("/my file", false, "hello\n");
    card_add("/plain", false, "plain\n");
}

#define CHECK_OUT(expected) do {                                                            \
        CHECK(!strcmp(out, (expected)));                                                    \
        if (strcmp(out, (expected)))                                                        \
            fprintf(stderr, "  output: \"%s\"\n  wanted: \"%s\"\n", out, (expected));       \
    } while (0)

// --- the tables' rule ----------------------------------------------------------------------------

static void t_run(shell_t *s, char *args) { (void)s; (void)args; }
static void t_runv(shell_t *s, int argc, char **argv) { (void)s; (void)argc; (void)argv; }

TEST(files_commands_follow_the_table_rule) {
    const shell_cmd_t *const tables[] = {files_commands, NULL};
    CHECK(shell_tables_check(tables) == NULL);
    // The path commands of files.c, by name (docs/design/shell-quoting.md §4): each takes words.
    static const char *const paths[] = {"ls", "cd", "cat", "cp", "mv", "rm", "mkdir"};
    int found = 0;
    for (const shell_cmd_t *c = files_commands; c->name; c++) {
        bool path = false;
        for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
            path |= !strcmp(c->name, paths[i]);
        CHECK_EQ(!!(c->flags & SHELL_PATH), path);
        CHECK(!path || c->runv);
        found += path;
    }
    CHECK_EQ(found, 7);
}

TEST(table_check_finds_the_entry_that_breaks_the_rule) {
    static const shell_cmd_t good[] = {
        {"a", t_run, NULL, NULL, 0}, {"b", NULL, t_runv, NULL, SHELL_PATH}, {0}};
    static const shell_cmd_t both[] = {
        {"ok", t_run, NULL, NULL, 0}, {"both", t_run, t_runv, NULL, 0}, {0}};
    static const shell_cmd_t none[] = {{"none", NULL, NULL, NULL, 0}, {0}};
    static const shell_cmd_t flat_path[] = {
        {"flat", t_run, NULL, NULL, SHELL_PATH | SHELL_DIR}, {0}};
    const shell_cmd_t *const t_good[] = {good, files_commands, NULL};
    const shell_cmd_t *const t_both[] = {good, both, NULL};
    const shell_cmd_t *const t_none[] = {none, good, NULL};
    const shell_cmd_t *const t_flat[] = {good, flat_path, NULL};
    const shell_cmd_t *const t_empty[] = {NULL};
    CHECK(shell_tables_check(t_good) == NULL);
    CHECK(shell_tables_check(t_both) == &both[1]);
    CHECK(shell_tables_check(t_none) == &none[0]);
    CHECK(shell_tables_check(t_flat) == &flat_path[0]);
    CHECK(shell_tables_check(t_empty) == NULL);
}

// --- the commands' words -------------------------------------------------------------------------

TEST(cat_takes_a_quoted_or_escaped_name_with_a_space) {
    setup();
    run("cat 'my file'");
    CHECK_OUT("hello\n");
    run("cat my\\ file");
    CHECK_OUT("hello\n");
    run("cat \"/my file\"");
    CHECK_OUT("hello\n");
}

TEST(cat_wants_exactly_one_word) {
    setup();
    run("cat my file");             // two names, as in sh
    CHECK_OUT("usage: cat <file>\n");
    run("cat");
    CHECK_OUT("usage: cat <file>\n");
    run("cat ''");                  // an empty word names nothing
    CHECK_OUT("usage: cat <file>\n");
    run("cat nothing");
    CHECK_OUT("cat: nothing: no such file\n");
}

TEST(cp_and_mv_take_two_words_with_spaces) {
    setup();
    run("cp \"my file\" 'copy of it'");
    CHECK(!strncmp(out, "copied 6 bytes", 14));
    CHECK(card_find("/copy of it") >= 0);
    CHECK(!strcmp(card[card_find("/copy of it")].data, "hello\n"));
    run("mv 'copy of it' moved\\ here");
    CHECK_OUT("");
    CHECK(card_find("/copy of it") < 0);
    CHECK(card_find("/moved here") >= 0);
    run("cp a b c");
    CHECK_OUT("usage: cp <from> <to>\n");
    run("cp plain");
    CHECK_OUT("usage: cp <from> <to>\n");
    run("cp plain ''");
    CHECK_OUT("usage: cp <from> <to>\n");
    run("mv 'my file'");
    CHECK_OUT("usage: mv <from> <to>\n");
    run("mv a b c");
    CHECK_OUT("usage: mv <from> <to>\n");
}

TEST(cp_into_a_directory_with_a_space) {
    setup();
    card_add("/new dir", true, NULL);
    run("cp plain 'new dir'");
    CHECK(card_find("/new dir/plain") >= 0);
    run("mv 'my file' 'new dir'");
    CHECK(card_find("/new dir/my file") >= 0);
}

TEST(mkdir_cd_and_rm_take_one_word) {
    setup();
    run("mkdir 'new dir'");
    CHECK_OUT("");
    CHECK(card_find("/new dir") >= 0 && card[card_find("/new dir")].dir);
    run("cd 'new dir'");
    CHECK_OUT("");
    CHECK(!strcmp(sh.cwd, "/new dir"));
    run("cd");                      // no word: the root
    CHECK(!strcmp(sh.cwd, "/"));
    run("cd new dir");
    CHECK_OUT("usage: cd [dir]\n");
    CHECK(!strcmp(sh.cwd, "/"));
    run("rm 'my file'");
    CHECK_OUT("");
    CHECK(card_find("/my file") < 0);
    run("rm a b");
    CHECK_OUT("usage: rm <file or empty directory>\n");
    run("rm");
    CHECK_OUT("usage: rm <file or empty directory>\n");
    run("mkdir");
    CHECK_OUT("usage: mkdir <dir>\n");
    run("mkdir a b");
    CHECK_OUT("usage: mkdir <dir>\n");
    CHECK(card_find("/a") < 0 && card_find("/b") < 0);
}

TEST(ls_takes_at_most_one_word) {
    setup();
    run("ls 'my file'");
    CHECK_OUT("           6  my file\n");
    run("ls");                      // the card's directories list now (#218): setup()'s two files
    CHECK_OUT("           6  2026-11-27 17:42 UTC  my file\n"
              "           6  2026-11-27 17:42 UTC  plain\n"
              "2 files, 0 directories, 12 bytes\n");
    CHECK(!strcmp(listing, "/"));
    run("ls a b");
    CHECK_OUT("usage: ls [path]\n");
}

// --- a file being written (#218) -----------------------------------------------------------------

// The line of `ls` for `name` in `out`, as a pointer into it, or NULL.
static const char *ls_line(const char *name) {
    for (const char *l = out; *l; l = strchr(l, '\n') + 1) {
        const char *nl = strchr(l, '\n');
        size_t n = strlen(name);
        if ((size_t)(nl - l) > n && !strncmp(nl - n, name, n) && nl[-(long)n - 1] == ' ')
            return l;
    }
    return NULL;
}

TEST(ls_says_being_written_for_a_zero_date_and_keeps_the_columns) {
    setup();
    int i = card_add("/new", false, "abc");
    card[i].entry_len = 0;          // as FatFs leaves a file it is writing: size 0, no date
    card[i].fdate = card[i].ftime = 0;
    card_add("/sub", true, NULL);
    card[card_find("/sub")].fdate = 0;
    run("ls");
    CHECK(strstr(out, "           0  being written         new\n") != NULL);
    CHECK(strstr(out, "       <dir>                        sub/\n") != NULL);   // no date, no phrase
    const char *a = ls_line("plain"), *b = ls_line("new");
    CHECK(a && b && strchr(a, '\n') - a - 5 == strchr(b, '\n') - b - 3);   // names line up
    CHECK(strstr(out, "3 files, 1 directories, 12 bytes\n") != NULL);
}

TEST(redirect_syncs_the_file_once_a_second_and_not_before) {
    setup();
    tick = 0;                       // the test moves the clock
    CHECK(open_stages("> /log"));
    pipe_write(&sh.pipe, "a\n", 2);  // opens the file: the second starts now
    run("ls");
    CHECK(strstr(out, "           0  being written         log\n") != NULL);
    now += 999999;
    pipe_write(&sh.pipe, "b\n", 2);
    CHECK_EQ(nsyncs, 0);            // not before a second has passed
    now += 1;
    pipe_write(&sh.pipe, "c\n", 2);
    CHECK_EQ(nsyncs, 1);
    CHECK_EQ(syncs[0].len, 6);
    run("ls");
    CHECK(strstr(out, "           6  2026-11-28 09:05 UTC  log\n") != NULL);
    out_clear();
    for (int k = 0; k < 50; k++) {  // a fast producer: no sync until the next second's mark
        now += 10000;
        pipe_write(&sh.pipe, "d\n", 2);
    }
    CHECK_EQ(nsyncs, 1);
    now += 500000;
    pipe_write(&sh.pipe, "e\n", 2);
    CHECK_EQ(nsyncs, 2);
    CHECK_EQ(syncs[1].at - syncs[0].at, 1000000);
    pipe_end(&sh.pipe);
    CHECK_EQ(nsyncs, 2);            // the close writes the rest out, without a sync
    CHECK_EQ(card[card_find("/log")].entry_len, 108);
    CHECK_OUT("");                  // nothing said, nothing passed on
}

TEST(tee_ends_once_when_the_sync_fails) {
    setup();
    tick = 0;
    sync_result = FR_DISK_ERR;
    CHECK(open_stages("| tee /log"));
    pipe_write(&sh.pipe, "a\n", 2);
    now += 1000000;
    pipe_write(&sh.pipe, "b\n", 2);  // passed on, then the sync fails: said once, the line ends
    pipe_write(&sh.pipe, "c\n", 2);
    pipe_end(&sh.pipe);
    CHECK_EQ(nsyncs, 1);
    CHECK_OUT("a\nb\ntee: /log: error\n");
}

TEST(a_typed_redirect_writes_the_file_and_dates_it) {
    setup();
    run("echo hello there > /f");
    CHECK_OUT("");
    CHECK(!strcmp(card[card_find("/f")].data, "hello there\n"));
    CHECK_EQ(nsyncs, 0);            // a short line: the close writes it out
    run("ls");
    CHECK(strstr(out, "          12  2026-11-28 09:05 UTC  f\n") != NULL);
}

static void fill(char *buf, size_t n) {
    for (size_t k = 0; k < n; k++)
        buf[k] = (char)('a' + k % 26);
    buf[n] = '\0';
}

TEST(cp_syncs_a_long_copy_once_a_second) {
    setup();
    char data[201];
    fill(data, 200);
    card_add("/big", false, data);
    read_max = 1;                   // a byte a read and 5 ms a clock reading: about 25 ms and
    tick = 5000;                    // 3 bytes a step, 67 steps, 1.7 s in all
    uint64_t start = now;
    run("cp big copy");
    CHECK(!strncmp(out, "copied 200 bytes", 16));
    CHECK(!strcmp(card[card_find("/copy")].data, data));
    CHECK_EQ(nsyncs, 1);            // once, at the second's mark, mid-copy
    CHECK(syncs[0].at - start >= 1000000 && syncs[0].at - start < 1000000 + 30000);
    CHECK(syncs[0].len > 0 && syncs[0].len < 200);
    CHECK_EQ(syncs[0].slot, card_find("/copy"));
}

TEST(cp_fails_when_the_sync_fails) {
    setup();
    char data[201];
    fill(data, 200);
    card_add("/big", false, data);
    read_max = 1;
    tick = 5000;
    sync_result = FR_DISK_ERR;
    run("cp big copy");
    CHECK(!strncmp(out, "cp: error\ncp: stopped after ", 28));
    CHECK(strstr(out, " of 200 bytes; partial copy removed\n") != NULL);
    CHECK(card_find("/copy") < 0);
}

TEST(echo_and_pwd_still_get_the_joined_text) {
    setup();
    run("echo a   'b  c'");
    CHECK_OUT("a b  c\n");
    run("echo");
    CHECK_OUT("\n");
    run("pwd");
    CHECK_OUT("/\n");
}

TEST_MAIN(
    RUN(files_commands_follow_the_table_rule);
    RUN(table_check_finds_the_entry_that_breaks_the_rule);
    RUN(cat_takes_a_quoted_or_escaped_name_with_a_space);
    RUN(cat_wants_exactly_one_word);
    RUN(cp_and_mv_take_two_words_with_spaces);
    RUN(cp_into_a_directory_with_a_space);
    RUN(mkdir_cd_and_rm_take_one_word);
    RUN(ls_takes_at_most_one_word);
    RUN(ls_says_being_written_for_a_zero_date_and_keeps_the_columns);
    RUN(redirect_syncs_the_file_once_a_second_and_not_before);
    RUN(tee_ends_once_when_the_sync_fails);
    RUN(a_typed_redirect_writes_the_file_and_dates_it);
    RUN(cp_syncs_a_long_copy_once_a_second);
    RUN(cp_fails_when_the_sync_fails);
    RUN(echo_and_pwd_still_get_the_joined_text);
)
