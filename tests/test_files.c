// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/files/files.c: the file commands' arguments, run from typed lines the way
// the shell runs them (words_split(), then shell.c's dispatch), over a card in memory that
// stands in for FatFs (tests/stubs/ff.h). And the command tables' rule (shell_tables_check(),
// src/shell/cmdcheck.c) over files_commands and over tables made to break it.

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

static struct {
    char path[256];             // absolute; "" for a free slot
    bool dir;
    char data[256];
    size_t len;
} card[CARD_FILES];

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
            card[i].len = strlen(card[i].data);
            return i;
        }
    return -1;
}

uint64_t time_us_64(void) {
    static uint64_t now;
    return now += 1000;
}

FSIZE_t test_f_size(const FIL *fp) {
    return card[fp->slot].len;
}

FRESULT f_open(FIL *fp, const char *path, uint8_t mode) {
    int i = card_find(path);
    if (i >= 0 && card[i].dir)
        return FR_DENIED;
    if (mode & FA_WRITE) {
        if (i < 0)
            i = card_add(path, false, "");
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

FRESULT f_close(FIL *fp) {
    (void)fp;
    return FR_OK;
}

FRESULT f_read(FIL *fp, void *buf, UINT btr, UINT *br) {
    size_t left = card[fp->slot].len - fp->pos;
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
    fno->fsize = card[i].len;
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

FRESULT f_readdir(DIR *dp, FILINFO *fno) {
    (void)dp;
    fno->fname[0] = '\0';       // every directory reads as empty: ls's listing isn't under test
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

void shell_write(shell_t *sh, const char *buf, size_t len) {
    (void)sh;
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

// The line as the shell runs it: split into words, then shell.c's shell_execute() over
// files_commands (runv gets the words, run their join).
static void run(const char *line) {
    char buf[SHELL_LINE_MAX + 1];
    words_t w;
    snprintf(buf, sizeof(buf), "%s", line);
    out_len = 0;
    out[0] = '\0';
    CHECK(words_split(buf, &w) == NULL);
    for (const shell_cmd_t *c = files_commands; c->name; c++)
        if (!strcmp(w.v[0], c->name)) {
            if (c->runv)
                c->runv(&sh, w.n, w.v);
            else
                c->run(&sh, words_join(w.n, w.v));
            return;
        }
    CHECK(!"no such command");
}

static void setup(void) {
    card_clear();
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
    run("ls");
    CHECK_OUT("0 files, 0 directories, 0 bytes\n");
    CHECK(!strcmp(listing, "/"));
    run("ls a b");
    CHECK_OUT("usage: ls [path]\n");
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
    RUN(echo_and_pwd_still_get_the_joined_text);
)
