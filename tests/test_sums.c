// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/files/sums.c: sha1sum, sha256sum and sha512sum run from typed lines the
// way the shell runs them, with wolfCrypt built for the host, over a card in memory standing in
// for FatFs (tests/stubs/ff.h). Digests against FIPS 180 vectors; -c with each result, names
// relative to the list, --ignore-missing, waiting for output room, Ctrl-C, and the files
// released at the end.

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

#include "ff.h"
#include "pico/stdlib.h"

#include "files/files.h"
#include "files/sdcard.h"
#include "files/sums.h"
#include "files/sums_parse.h"
#include "shell/shell.h"
#include "shell/words.h"
#include "test.h"

// --- a card in memory, read only -----------------------------------------------------------------

#define CARD_FILES 16

static struct {
    char path[256];             // absolute; "" for a free slot
    bool dir;
    char *data;
    size_t len;
} card[CARD_FILES];

static int open_files;
static FRESULT read_result;     // what f_read() returns
static int read_fail_after;     // f_read() fails after this many good reads (0: never)
static int reads;

static void card_clear(void) {
    for (int i = 0; i < CARD_FILES; i++)
        free(card[i].data);
    memset(card, 0, sizeof(card));
}

static int card_find(const char *path) {
    for (int i = 0; i < CARD_FILES; i++)
        if (card[i].path[0] && !strcmp(card[i].path, path))
            return i;
    return -1;
}

static void card_put(const char *path, const char *data, size_t len) {
    for (int i = 0; i < CARD_FILES; i++)
        if (!card[i].path[0]) {
            snprintf(card[i].path, sizeof(card[i].path), "%s", path);
            card[i].dir = data == NULL;
            card[i].data = malloc(len + 1);
            if (data)
                memcpy(card[i].data, data, len);
            card[i].len = len;
            return;
        }
    CHECK(!"card full");
}

static void card_add(const char *path, const char *text) {
    card_put(path, text, text ? strlen(text) : 0);
}

static uint64_t now, tick = 1000;

uint64_t time_us_64(void) {
    return now += tick;
}

FSIZE_t test_f_size(const FIL *fp) {
    return card[fp->slot].len;
}

FRESULT f_open(FIL *fp, const char *path, BYTE mode) {
    CHECK(mode == FA_READ);
    int i = card_find(path);
    if (i < 0 || card[i].dir) {         // FatFs: FR_NO_FILE for a directory too
        char parent[256];
        snprintf(parent, sizeof(parent), "%s", path);
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) {
            *slash = '\0';
            if (card_find(parent) < 0)
                return FR_NO_PATH;
        }
        return FR_NO_FILE;
    }
    fp->slot = i;
    fp->pos = 0;
    fp->mode = mode;
    open_files++;
    return FR_OK;
}

FRESULT f_close(FIL *fp) {
    (void)fp;
    open_files--;
    return FR_OK;
}

FRESULT f_read(FIL *fp, void *buf, UINT btr, UINT *br) {
    if (read_fail_after && ++reads > read_fail_after)
        return read_result;
    size_t left = card[fp->slot].len - fp->pos;
    *br = btr < left ? btr : (UINT)left;
    memcpy(buf, card[fp->slot].data + fp->pos, *br);
    fp->pos += *br;
    return FR_OK;
}

FRESULT f_stat(const char *path, FILINFO *fno) {
    int i = card_find(path);
    if (i < 0)
        return FR_NO_FILE;
    memset(fno, 0, sizeof(*fno));
    fno->fsize = card[i].len;
    fno->fattrib = card[i].dir ? AM_DIR : 0;
    return FR_OK;
}

// --- what sums.c uses from files.c and sdcard.c --------------------------------------------------

static int holders;

void files_hold(void) {
    holders++;
}

void files_release(void) {
    holders--;
}

const char *files_error(int fr) {
    switch ((FRESULT)fr) {
    case FR_NO_FILE: return "no such file";
    case FR_NO_PATH: return "no such directory";
    case FR_DISK_ERR: return "disk error";
    default:         return "error";
    }
}

bool sdcard_mount(shell_t *sh) {
    (void)sh;
    return true;
}

// --- a shell: its output, and jobs stepped to the end --------------------------------------------

static char out[16384];
static size_t out_len;
static int space = 1024;        // shell_write_space()
static int steps, cancel_at;    // cancel_at: Ctrl-C before this step (0: never)

void shell_write(shell_t *s, const char *buf, size_t len) {
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
    return space;
}

void shell_run_job(shell_t *sh, shell_job_fn step, void *arg) {
    for (steps = 1;; steps++) {
        if (cancel_at && steps == cancel_at) {
            CHECK(!step(sh, arg, true));
            return;
        }
        if (!step(sh, arg, false))
            return;
        CHECK(steps < 100000);
    }
}

static const shell_io_t io_console = {.name = "console", .admin = true};
static shell_t sh;

static void run(const char *line) {
    char buf[SHELL_LINE_MAX + 1];
    words_t w;
    snprintf(buf, sizeof(buf), "%s", line);
    out_len = 0;
    out[0] = '\0';
    CHECK(words_split(buf, &w) == NULL);
    for (const shell_cmd_t *c = sums_commands; c->name; c++)
        if (!strcmp(w.v[0], c->name)) {
            c->runv(&sh, w.n, w.v);
            CHECK_EQ(holders, 0);           // released at the end
            CHECK_EQ(open_files, 0);
            return;
        }
    CHECK(!"no such command");
}

#define CHECK_OUT(expected) do {                                                            \
        CHECK(!strcmp(out, (expected)));                                                    \
        if (strcmp(out, (expected)))                                                        \
            fprintf(stderr, "  output: \"%s\"\n  wanted: \"%s\"\n", out, (expected));       \
    } while (0)

#define ABC1   "a9993e364706816aba3e25717850c26c9cd0d89d"
#define ABC256 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
#define ABC512 "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a" \
               "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"
#define EMPTY256 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
#define MILLION256 "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"

static void setup(void) {
    card_clear();
    open_files = holders = 0;
    read_result = FR_OK;
    read_fail_after = reads = 0;
    space = 1024;
    cancel_at = 0;
    tick = 1000;
    memset(&sh, 0, sizeof(sh));
    sh.io = &io_console;
    strcpy(sh.cwd, "/");
    card_add("/abc", "abc");
    card_add("/empty", "");
    card_add("/apps", NULL);
    card_add("/apps/abc", "abc");
}

// --- the table ----------------------------------------------------------------------------------

TEST(commands_follow_the_table_rule) {
    const shell_cmd_t *const tables[] = {sums_commands, NULL};
    CHECK(shell_tables_check(tables) == NULL);
    int n = 0;
    for (const shell_cmd_t *c = sums_commands; c->name; c++, n++)
        CHECK(c->runv && (c->flags & SHELL_PATH));
    CHECK_EQ(n, 3);
}

// --- printing -----------------------------------------------------------------------------------

TEST(each_algorithm_prints_the_fips_vector) {
    setup();
    run("sha1sum abc");
    CHECK_OUT(ABC1 "  abc\n");
    run("sha256sum abc");
    CHECK_OUT(ABC256 "  abc\n");
    run("sha512sum abc");
    CHECK_OUT(ABC512 "  abc\n");
    run("sha256sum empty");
    CHECK_OUT(EMPTY256 "  empty\n");
}

TEST(names_are_printed_as_given_and_the_rest_continue) {
    setup();
    strcpy(sh.cwd, "/apps");
    run("sha256sum abc ../abc nothing /abc");
    CHECK_OUT(ABC256 "  abc\n" ABC256 "  ../abc\nsha256sum: nothing: no such file\n"
              ABC256 "  /abc\n");
    run("sha256sum /apps");
    CHECK_OUT("sha256sum: /apps: is a directory\n");
    run("sha256sum /");
    CHECK_OUT("sha256sum: /: is a directory\n");
}

TEST(a_big_file_is_hashed_over_many_steps) {
    setup();
    static char million[1000000];
    memset(million, 'a', sizeof(million));
    card_put("/big", million, sizeof(million));
    run("sha256sum big");
    CHECK_OUT(MILLION256 "  big\n");
    CHECK(steps > 10);                  // 4 KB per read, 20 ms per step, 1 ms per clock read
}

TEST(names_with_backslash_are_escaped_and_control_characters_refused) {
    setup();
    card_add("/a\\b", "abc");
    run("sha256sum 'a\\b'");
    CHECK_OUT("\\" ABC256 "  a\\\\b\n");
    char line[64];
    snprintf(line, sizeof(line), "sha256sum 'a%cb'", 0x1b);
    run(line);
    CHECK_OUT("sha256sum: a?b: name has control characters\n");
}

TEST(a_read_error_is_said) {
    setup();
    read_result = FR_DISK_ERR;
    read_fail_after = 1;
    static char data[10000];
    memset(data, 'x', sizeof(data));
    card_put("/long", data, sizeof(data));
    run("sha256sum long abc");
    CHECK_OUT("sha256sum: long: disk error\nsha256sum: abc: disk error\n");
}

TEST(output_waits_for_room) {
    setup();
    space = 10;
    cancel_at = 50;                     // never gets room: Ctrl-C ends it
    run("sha256sum abc");
    CHECK_OUT("");
    CHECK_EQ(steps, 50);
    space = 1024;
    cancel_at = 0;
    run("sha256sum abc");
    CHECK_OUT(ABC256 "  abc\n");
}

TEST(ctrl_c_stops_a_hash) {
    setup();
    static char million[1000000];
    card_put("/big", million, sizeof(million));
    cancel_at = 3;
    run("sha256sum big abc");
    CHECK_OUT("");
}

TEST(usage_and_options) {
    setup();
    run("sha256sum");
    CHECK_OUT("usage: sha256sum <file>...\n       sha256sum -c [--ignore-missing] <list>\n");
    run("sha256sum ''");
    CHECK_OUT("usage: sha256sum <file>...\n       sha256sum -c [--ignore-missing] <list>\n");
    run("sha1sum -x abc");
    CHECK_OUT("sha1sum: unknown option -x\n"
              "usage: sha1sum <file>...\n       sha1sum -c [--ignore-missing] <list>\n");
    run("sha256sum -c a b");
    CHECK_OUT("usage: sha256sum <file>...\n       sha256sum -c [--ignore-missing] <list>\n");
    run("sha256sum --ignore-missing abc");
    CHECK_OUT("sha256sum: --ignore-missing is for -c\n"
              "usage: sha256sum <file>...\n       sha256sum -c [--ignore-missing] <list>\n");
    card_add("/-c", "abc");
    run("sha256sum -- -c");
    CHECK_OUT(ABC256 "  -c\n");
}

// --- checking -----------------------------------------------------------------------------------

TEST(check_says_each_result_and_sums_them_up) {
    setup();
    card_add("/apps/bad", "not abc");
    card_add("/apps/SUMS",
             "# made by hand\n"
             ABC256 "  abc\n"
             "\n"
             ABC256 " *bad\n"
             ABC256 "  gone\n"
             "SHA256 (abc) = " ABC256 "\r\n"
             "garbage\n"
             "SHA1 (abc) = " ABC1 "\n"
             ABC256 "  ../abc\n"
             ABC256 "  /abc\n");
    run("sha256sum -c /apps/SUMS");
    CHECK_OUT("abc: OK\nbad: FAILED\ngone: missing\nabc: OK\n../abc: unsafe path\n"
              "/abc: unsafe path\n2 OK, 1 FAILED, 1 missing, 2 unsafe, 2 improperly formatted\n");
}

TEST(check_names_are_relative_to_the_list) {
    setup();
    card_add("/apps/SUMS", ABC256 "  abc\n");
    run("sha256sum -c apps/SUMS");
    CHECK_OUT("abc: OK\n1 OK\n");
    strcpy(sh.cwd, "/apps");
    card_add("/SUMS", ABC256 "  apps/abc\n");
    run("sha256sum -c ../SUMS");
    CHECK_OUT("apps/abc: OK\n1 OK\n");
    run("sha256sum -c SUMS");
    CHECK_OUT("abc: OK\n1 OK\n");
}

TEST(check_ignore_missing) {
    setup();
    card_add("/SUMS", ABC256 "  abc\n" ABC256 "  other-platform.tar.gz\n");
    run("sha256sum -c --ignore-missing SUMS");
    CHECK_OUT("abc: OK\n1 OK\n");
    card_add("/NONE", ABC256 "  x\n" ABC256 "  y\n");
    run("sha256sum --ignore-missing -c NONE");
    CHECK_OUT("sha256sum: NONE: no file was verified (all missing)\n");
}

TEST(check_with_no_usable_line) {
    setup();
    card_add("/EMPTY", "");
    run("sha256sum -c EMPTY");
    CHECK_OUT("sha256sum: EMPTY: no checksums found\n");
    card_add("/JUNK", "# only\nnot a line\n" ABC1 "  abc\n");
    run("sha256sum -c JUNK");
    CHECK_OUT("sha256sum: JUNK: no checksums found\n");
    run("sha1sum -c JUNK");
    CHECK_OUT("abc: OK\n1 OK, 1 improperly formatted\n");
    run("sha256sum -c NOLIST");
    CHECK_OUT("sha256sum: NOLIST: no such file\n");
}

TEST(check_bsd_lines_for_each_algorithm) {
    setup();
    card_add("/S1", "SHA1 (abc) = " ABC1 "\n");
    card_add("/S512", "SHA512 (abc) = " ABC512 "\nSHA256 (abc) = " ABC256 "\n");
    run("sha1sum -c S1");
    CHECK_OUT("abc: OK\n1 OK\n");
    run("sha512sum -c S512");
    CHECK_OUT("abc: OK\n1 OK, 1 improperly formatted\n");
}

TEST(check_escaped_names) {
    setup();
    card_add("/a\\b", "abc");
    card_add("/SUMS", "\\" ABC256 "  a\\\\b\n\\" ABC256 "  new\\nline\n");
    run("sha256sum -c SUMS");
    CHECK_OUT("\\a\\\\b: OK\n\\new\\nline: unsafe path\n1 OK, 1 unsafe\n");
}

TEST(check_long_lines_are_said) {
    setup();
    static char list[SUMS_LINE_MAX + 200];
    memset(list, 'x', SUMS_LINE_MAX + 1);
    snprintf(list + SUMS_LINE_MAX + 1, 100, "\n%s  abc\n", ABC256);
    card_add("/SUMS", list);
    run("sha256sum -c SUMS");
    CHECK_OUT("line 1: too long\nabc: OK\n1 OK, 1 improperly formatted\n");
}

TEST(check_list_from_a_file_name_with_control_characters_is_shown_safely) {
    setup();
    card_add("/SUMS", ABC256 "  a\x1b[2Jb\n");
    run("sha256sum -c SUMS");
    CHECK_OUT("a?[2Jb: unsafe path\n0 OK, 1 unsafe\n");
}

TEST(check_ctrl_c_says_stopped) {
    setup();
    static char million[1000000];
    card_put("/big", million, sizeof(million));
    card_add("/SUMS", ABC256 "  big\n");
    cancel_at = 3;
    run("sha256sum -c SUMS");
    CHECK_OUT("sha256sum: stopped\n");
}

TEST(check_read_error_of_a_listed_file) {
    setup();
    static char data[10000];
    card_put("/long", data, sizeof(data));
    card_add("/SUMS", ABC256 "  long\n");
    read_result = FR_DISK_ERR;
    read_fail_after = 2;                // the list, then the file's first block
    run("sha256sum -c SUMS");
    CHECK_OUT("long: disk error\nsha256sum: SUMS: disk error\n0 OK, 1 unreadable\n");
}

TEST_MAIN(
    RUN(commands_follow_the_table_rule);
    RUN(each_algorithm_prints_the_fips_vector);
    RUN(names_are_printed_as_given_and_the_rest_continue);
    RUN(a_big_file_is_hashed_over_many_steps);
    RUN(names_with_backslash_are_escaped_and_control_characters_refused);
    RUN(a_read_error_is_said);
    RUN(output_waits_for_room);
    RUN(ctrl_c_stops_a_hash);
    RUN(usage_and_options);
    RUN(check_says_each_result_and_sums_them_up);
    RUN(check_names_are_relative_to_the_list);
    RUN(check_ignore_missing);
    RUN(check_with_no_usable_line);
    RUN(check_bsd_lines_for_each_algorithm);
    RUN(check_escaped_names);
    RUN(check_long_lines_are_said);
    RUN(check_list_from_a_file_name_with_control_characters_is_shown_safely);
    RUN(check_ctrl_c_says_stopped);
    RUN(check_read_error_of_a_listed_file);
    card_clear();
)
