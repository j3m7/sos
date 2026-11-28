// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/files/gzip.c: gunzip and zcat as the shell runs them, over a card in memory
// that stands in for FatFs (tests/stubs/ff.h), with the real files.c, gz.c, bigjob.c and zlib,
// on the files of tests/data/archives/. The output name, -k and -f, the .part renamed only on
// success and deleted on an error, Ctrl-C and a full card; trailing zeros; zcat's output paced
// by the terminal's space; the memory check, the one-at-a-time rule, and the heap given back.

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

#include "ff.h"
#include "f_util.h"
#include "pico/stdlib.h"

#include "core/heap.h"
#include "core/heaptag.h"
#include "files/bigjob.h"
#include "files/files.h"
#include "files/gzip.h"
#include "files/sdcard.h"
#include "shell/complete.h"
#include "shell/shell.h"
#include "test.h"

#ifndef ARCHIVES_DIR
#error "ARCHIVES_DIR: tests/data/archives"
#endif

// --- a card in memory -------------------------------------------------------------------------

#define CARD_FILES 16

static struct {
    char path[256];             // absolute; "" for a free slot
    bool dir;
    uint8_t *data;
    size_t len;
} card[CARD_FILES];
static size_t card_room;        // bytes the card holds in all (a full card: writes stop short)
static int syncs;

static size_t card_used(void) {
    size_t n = 0;
    for (int i = 0; i < CARD_FILES; i++)
        if (card[i].path[0])
            n += card[i].len;
    return n;
}

static void card_clear(void) {
    for (int i = 0; i < CARD_FILES; i++)
        free(card[i].data);
    memset(card, 0, sizeof(card));
    card_room = 64u << 20;
    syncs = 0;
}

static int card_find(const char *path) {
    for (int i = 0; i < CARD_FILES; i++)
        if (card[i].path[0] && !strcmp(card[i].path, path))
            return i;
    return -1;
}

static int card_add(const char *path, bool dir, const void *data, size_t len) {
    for (int i = 0; i < CARD_FILES; i++)
        if (!card[i].path[0]) {
            snprintf(card[i].path, sizeof(card[i].path), "%s", path);
            card[i].dir = dir;
            free(card[i].data);
            card[i].data = malloc(len ? len : 1);
            memcpy(card[i].data, data, len);
            card[i].len = len;
            return i;
        }
    return -1;
}

// A file of tests/data/archives/ put on the card as `path`.
static void card_put(const char *path, const char *fixture) {
    char name[512];
    snprintf(name, sizeof(name), "%s/%s", ARCHIVES_DIR, fixture);
    FILE *f = fopen(name, "rb");
    if (!f) {
        fprintf(stderr, "%s: can't open\n", name);
        exit(1);
    }
    static uint8_t buf[1 << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    CHECK(card_add(path, false, buf, n) >= 0);
}

// Does the card's `path` hold what the fixture holds?
static bool card_is(const char *path, const char *fixture) {
    char name[512];
    snprintf(name, sizeof(name), "%s/%s", ARCHIVES_DIR, fixture);
    FILE *f = fopen(name, "rb");
    if (!f)
        return false;
    static uint8_t buf[1 << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    int i = card_find(path);
    return i >= 0 && card[i].len == n && !memcmp(card[i].data, buf, n);
}

static uint64_t now, tick;      // the clock moves `tick` on each reading

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
        if (i < 0)
            i = card_add(path, false, "", 0);
        if (i < 0)
            return FR_DENIED;
        card[i].len = 0;
    } else if (i < 0) {
        return FR_NO_FILE;
    }
    fp->slot = i;
    fp->mode = mode;
    fp->pos = 0;
    return FR_OK;
}

FRESULT f_close(FIL *fp) {
    (void)fp;
    return FR_OK;
}

FRESULT f_sync(FIL *fp) {
    (void)fp;
    syncs++;
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
    size_t used = card_used();
    size_t room = card_room > used ? card_room - used : 0;
    *bw = btw < room ? btw : (UINT)room;
    size_t end = fp->pos + *bw;
    card[fp->slot].data = realloc(card[fp->slot].data, end ? end : 1);
    memcpy(card[fp->slot].data + fp->pos, buf, *bw);
    fp->pos = end;
    if (end > card[fp->slot].len)
        card[fp->slot].len = end;
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

// files.c's other commands aren't run here.
FRESULT f_lseek(FIL *fp, FSIZE_t ofs) { fp->pos = ofs; return FR_OK; }
FRESULT f_opendir(DIR *dp, const char *path) { (void)dp; (void)path; return FR_NO_PATH; }
FRESULT f_readdir(DIR *dp, FILINFO *fno) { (void)dp; fno->fname[0] = '\0'; return FR_OK; }
FRESULT f_closedir(DIR *dp) { (void)dp; return FR_OK; }
FRESULT f_mkdir(const char *path) { (void)path; return FR_DENIED; }

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

static size_t heap_free_now;    // what heap_free() says

size_t heap_free(void) {
    return heap_free_now;
}

// --- a shell ------------------------------------------------------------------------------------

static char *out;
static size_t out_len, out_cap;
static int write_space;         // what shell_write_space() says
static int cancel_after;        // shell_run_job cancels the job after this many steps (0: never)
static int steps;

void shell_write(shell_t *s, const char *buf, size_t len) {
    (void)s;
    CHECK(len <= (size_t)write_space);
    while (out_len + len + 1 > out_cap)
        out = realloc(out, out_cap = out_cap ? 2 * out_cap : 4096);
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
    int keep = write_space;
    write_space = 1 << 20;                  // messages aren't paced in this test
    if (n > 0)
        shell_write(sh, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    write_space = keep;
}

int shell_write_space(shell_t *sh) {
    (void)sh;
    return write_space;
}

void shell_run_job(shell_t *sh, shell_job_fn step, void *arg) {
    steps = 0;
    while (true) {
        if (cancel_after && steps == cancel_after) {
            CHECK(!step(sh, arg, true));
            return;
        }
        steps++;
        if (!step(sh, arg, false))
            return;
    }
}

static const shell_io_t io_console = {.name = "console", .admin = true};
static const shell_io_t io_ssh = {.name = "ssh", .admin = false};
static shell_t sh;

static void out_clear(void) {
    out_len = 0;
    if (out)
        out[0] = '\0';
}

// Run a line: split at spaces (the names here have none), then gzip_commands' handler.
static void run(const char *line) {
    char buf[256];
    char *argv[16];
    int argc = 0;
    snprintf(buf, sizeof(buf), "%s", line);
    for (char *w = strtok(buf, " "); w && argc < 15; w = strtok(NULL, " "))
        argv[argc++] = w;
    argv[argc] = NULL;
    out_clear();
    for (const shell_cmd_t *c = gzip_commands; c->name; c++)
        if (!strcmp(argv[0], c->name)) {
            c->runv(&sh, argc, argv);
            return;
        }
    fprintf(stderr, "  no such command: %s\n", argv[0]);
    CHECK(false);
}

static void setup(void) {
    card_clear();
    heap_free_now = 300 * 1024;
    write_space = 4096;
    cancel_after = 0;
    tick = 1000;
    memset(&sh, 0, sizeof(sh));
    sh.io = &io_console;
    strcpy(sh.cwd, "/");
}

#define CHECK_HAS(text) do {                                                                \
        CHECK(out && strstr(out, (text)));                                                  \
        if (!out || !strstr(out, (text)))                                                   \
            fprintf(stderr, "  output: \"%s\"\n  wanted in it: \"%s\"\n", out ? out : "", (text)); \
    } while (0)

// Nothing held when no job runs: heap, open files, the one-job slot.
static void check_released(void) {
    CHECK_EQ(heap_tag_used(HEAP_FILES), 0);
    CHECK_EQ(heap_tag_used(HEAP_ZLIB), 0);
    CHECK(!files_busy());
    CHECK(bigjob_running() == NULL);
}

// --- tests --------------------------------------------------------------------------------------

TEST(the_table_follows_the_rule) {
    const shell_cmd_t *const tables[] = {gzip_commands, NULL};
    CHECK(shell_tables_check(tables) == NULL);
}

TEST(gunzip_replaces_x_gz_with_x) {
    setup();
    card_put("/text.txt.gz", "text.txt.gz");
    run("gunzip text.txt.gz");
    CHECK(card_is("/text.txt", "text.txt"));
    CHECK(card_find("/text.txt.gz") < 0);
    CHECK(card_find("/text.txt.part") < 0);
    CHECK_HAS("text.txt: 28893 bytes in ");
    CHECK(steps > 1);                       // in steps, not all at once
    check_released();
}

TEST(gunzip_k_keeps_the_input_and_tgz_gives_tar) {
    setup();
    card_put("/d.tgz", "multi.gz");
    run("gunzip -k /d.tgz");
    CHECK(card_is("/d.tar", "text.txt"));
    CHECK(card_find("/d.tgz") >= 0);
    check_released();
}

TEST(gunzip_does_not_replace_without_f) {
    setup();
    card_put("/a.gz", "text.txt.gz");
    card_add("/a", false, "old", 3);
    run("gunzip a.gz");
    CHECK_HAS("gunzip: a: already exists (-f replaces it)");
    CHECK(card_find("/a.gz") >= 0);
    CHECK_EQ(card[card_find("/a")].len, 3);
    run("gunzip -f a.gz");
    CHECK(card_is("/a", "text.txt"));
    CHECK(card_find("/a.gz") < 0);
    card_put("/b.gz", "text.txt.gz");
    card_add("/b", true, "", 0);
    run("gunzip -kf b.gz");
    CHECK_HAS("gunzip: b: is a directory");
    check_released();
}

TEST(gunzip_goes_on_after_a_bad_file) {
    setup();
    card_put("/x.zip", "text.txt.gz");
    card_put("/bad.gz", "badcrc.gz");
    card_put("/good.gz", "text.txt.gz");
    run("gunzip x.zip bad.gz missing.gz good.gz");
    CHECK_HAS("gunzip: x.zip: unknown suffix (.gz or .tgz)\n");
    CHECK_HAS("gunzip: bad.gz: CRC error: the data is corrupt\n");
    CHECK_HAS("gunzip: missing.gz: no such file\n");
    CHECK(card_find("/bad") < 0 && card_find("/bad.part") < 0);
    CHECK(card_find("/bad.gz") >= 0);       // a failed input is kept
    CHECK(card_is("/good", "text.txt"));
    check_released();
}

TEST(gunzip_errors_delete_the_output) {
    static const char *const bad[] = {"junk.gz", "zerosjunk.gz", "badlen.gz", "truncated.gz",
                                      "empty.gz", "notgz.gz", "baddata.gz", "badmember.gz"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        setup();
        card_put("/f.gz", bad[i]);
        run("gunzip f.gz");
        CHECK_HAS("gunzip: f.gz: ");
        CHECK(card_find("/f") < 0 && card_find("/f.part") < 0);
        CHECK(card_find("/f.gz") >= 0);
        check_released();
    }
}

TEST(gunzip_notes_trailing_zeros) {
    setup();
    card_put("/z.gz", "zeros.gz");
    run("gunzip z.gz");
    CHECK_HAS("gunzip: z.gz: ignoring trailing zeros\n");
    CHECK(card_is("/z", "text.txt"));
    check_released();
}

TEST(ctrl_c_leaves_no_part) {
    setup();
    card_put("/big.gz", "zeros1m.gz");
    cancel_after = 3;
    run("gunzip big.gz");
    CHECK_HAS("gunzip: stopped; big.part removed\n");
    CHECK(card_find("/big.part") < 0 && card_find("/big") < 0);
    CHECK(card_find("/big.gz") >= 0);
    check_released();
}

TEST(a_full_card_stops_with_card_full) {
    setup();
    card_put("/big.gz", "zeros1m.gz");
    card_room = card_used() + 300000;
    tick = 10000;                           // two readings a step: a second in ~30 writes
    run("gunzip big.gz");
    CHECK_HAS("gunzip: big.gz: card full\n");
    CHECK(card_find("/big.part") < 0 && card_find("/big") < 0);
    CHECK(card_find("/big.gz") >= 0);
    CHECK(syncs > 0);                       // synced once a second meanwhile
    check_released();
}

TEST(zcat_prints_paced_by_the_terminal) {
    setup();
    card_put("/m.gz", "multi.gz");
    write_space = 300;                      // 150 bytes a write ("\n" -> "\r\n")
    run("zcat m.gz");
    CHECK(card_find("/m.gz") >= 0);
    CHECK(card_find("/m") < 0);
    char name[512];
    snprintf(name, sizeof(name), "%s/text.txt", ARCHIVES_DIR);
    FILE *f = fopen(name, "rb");
    static char want[65536];
    size_t n = fread(want, 1, sizeof(want), f);
    fclose(f);
    CHECK_EQ(out_len, n);
    CHECK(out_len == n && !memcmp(out, want, n));
    check_released();

    setup();
    card_put("/z.gz", "zeros.gz");          // quiet about padding: the output is the data
    run("zcat z.gz");
    CHECK_EQ(out_len, n);
    card_put("/j.gz", "junk.gz");
    run("zcat j.gz");
    CHECK_EQ(out_len, n + strlen("\nzcat: j.gz: trailing garbage after the data\n"));
    CHECK_HAS("\nzcat: j.gz: trailing garbage after the data\n");
    check_released();
}

TEST(memory_and_one_at_a_time) {
    setup();
    card_put("/a.gz", "text.txt.gz");
    heap_free_now = 100 * 1024;
    run("gunzip a.gz");
    CHECK_HAS("gunzip: not enough memory: needs ");
    CHECK(card_find("/a.gz") >= 0 && card_find("/a") < 0);
    heap_free_now = 300 * 1024;
    CHECK(bigjob_try_begin("fetch") == NULL);
    run("zcat a.gz");
    CHECK_HAS("busy: fetch is running in another shell\n");
    run("gunzip a.gz");
    CHECK_HAS("busy: fetch is running in another shell\n");
    bigjob_end();
    CHECK(card_find("/a.gz") >= 0 && card_find("/a") < 0);
    check_released();
}

TEST(protected_paths_and_usage) {
    setup();
    card_put("/certs/r.gz", "text.txt.gz");
    sh.io = &io_ssh;
    run("gunzip /certs/r.gz");
    CHECK_HAS("gunzip: /certs/r: not allowed to change this path\n");
    run("zcat /certs/r.gz");                // reading is allowed
    CHECK_HAS("6000\n");
    sh.io = &io_console;
    run("gunzip");
    CHECK_HAS("usage: gunzip [-k] [-f] <file>...\n");
    run("gunzip -x a.gz");
    CHECK_HAS("usage: gunzip");
    run("gunzip -k");
    CHECK_HAS("usage: gunzip");
    run("zcat");
    CHECK_HAS("usage: zcat <file>\n");
    run("zcat a b");
    CHECK_HAS("usage: zcat <file>\n");
    card_put("/-k.gz", "text.txt.gz");
    run("gunzip -- -k.gz");
    CHECK(card_is("/-k", "text.txt"));
    check_released();
}

TEST_MAIN(
    RUN(the_table_follows_the_rule);
    RUN(gunzip_replaces_x_gz_with_x);
    RUN(gunzip_k_keeps_the_input_and_tgz_gives_tar);
    RUN(gunzip_does_not_replace_without_f);
    RUN(gunzip_goes_on_after_a_bad_file);
    RUN(gunzip_errors_delete_the_output);
    RUN(gunzip_notes_trailing_zeros);
    RUN(ctrl_c_leaves_no_part);
    RUN(a_full_card_stops_with_card_full);
    RUN(zcat_prints_paced_by_the_terminal);
    RUN(memory_and_one_at_a_time);
    RUN(protected_paths_and_usage);
    card_clear();
    free(out);
)
