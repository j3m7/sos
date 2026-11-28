// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/files/tar.c: tar tf and tar xf as the shell runs them, over a card in memory
// that stands in for FatFs (tests/stubs/ff.h; names compared ignoring case, as FAT does), with the
// real tar_parse.c, gz.c, files.c, bigjob.c and zlib, on the archives of tests/data/archives/tar/.
// Listing (plain and gzipped, paced by the terminal); extracting GNU, pax, ustar, v7 and bsdtar
// archives with their dates; the safety rules on crafted archives (.., leading /, FAT names, case
// clashes, links, devices, protected paths, the archive itself); existing files without and with
// -f; Ctrl-C, a full card, damaged archives; -C; the memory check, the one-at-a-time rule, and the
// heap given back.

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>

#include "ff.h"
#include "f_util.h"
#include "pico/stdlib.h"

#include "core/heap.h"
#include "core/heaptag.h"
#include "files/bigjob.h"
#include "files/files.h"
#include "files/sdcard.h"
#include "files/tar.h"
#include "shell/shell.h"
#include "tar_tools.h"
#include "test.h"

#ifndef ARCHIVES_DIR
#error "ARCHIVES_DIR: tests/data/archives"
#endif

// --- a card in memory -------------------------------------------------------------------------

#define CARD_FILES 256
#define CARD_PATH  1100

static struct {
    char path[CARD_PATH];       // absolute; "" for a free slot
    bool dir;
    bool locked;                // open for writing elsewhere: f_open() says FR_LOCKED
    uint8_t *data;
    size_t len;
    uint16_t fdate, ftime;
} card[CARD_FILES];
static size_t card_room;        // bytes the card holds in all (a full card: writes stop short)
static bool exfat;

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
    exfat = false;
}

// FAT: names compare ignoring case.
static int card_find(const char *path) {
    for (int i = 0; i < CARD_FILES; i++)
        if (card[i].path[0] && !strcasecmp(card[i].path, path))
            return i;
    return -1;
}

// Exactly this spelling.
static int card_exact(const char *path) {
    int i = card_find(path);
    return i >= 0 && !strcmp(card[i].path, path) ? i : -1;
}

// Does the parent of `path` exist as a directory?
static bool parent_ok(const char *path) {
    char p[CARD_PATH];
    snprintf(p, sizeof(p), "%s", path);
    char *slash = strrchr(p, '/');
    if (slash == p)
        return true;
    *slash = '\0';
    int i = card_find(p);
    return i >= 0 && card[i].dir;
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
            card[i].fdate = card[i].ftime = 0;
            card[i].locked = false;
            return i;
        }
    fprintf(stderr, "  the card is out of slots\n");
    return -1;
}

static uint8_t *fixture(const char *name, size_t *len) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", ARCHIVES_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "%s: can't open\n", path);
        exit(1);
    }
    static uint8_t buf[1 << 20];
    *len = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return buf;
}

static void card_put(const char *path, const char *name) {
    size_t n;
    uint8_t *d = fixture(name, &n);
    CHECK(card_add(path, false, d, n) >= 0);
}

// Does the card's `path` (exactly spelled) hold `len` bytes of `data`?
static bool card_holds(const char *path, const void *data, size_t len) {
    int i = card_exact(path);
    return i >= 0 && !card[i].dir && card[i].len == len && !memcmp(card[i].data, data, len);
}

static bool card_text(const char *path, const char *text) {
    return card_holds(path, text, strlen(text));
}

static bool card_dir(const char *path) {
    int i = card_exact(path);
    return i >= 0 && card[i].dir;
}

static bool card_has_part(void) {
    for (int i = 0; i < CARD_FILES; i++) {
        size_t n = strlen(card[i].path);
        if (n > 5 && !strcmp(card[i].path + n - 5, ".part"))
            return true;
    }
    return false;
}

static uint64_t now, tick;

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
    if (i >= 0 && card[i].locked)
        return FR_LOCKED;
    if (mode & FA_WRITE) {
        if (!parent_ok(path))
            return FR_NO_PATH;
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

#define NOW_DATE ((2026 - 1980) << 9 | 11 << 5 | 27)    // what FatFs dates a file it closes

FRESULT f_close(FIL *fp) {
    if ((fp->mode & FA_WRITE) && card[fp->slot].path[0]) {
        card[fp->slot].fdate = NOW_DATE;
        card[fp->slot].ftime = 0;
    }
    return FR_OK;
}

FRESULT f_sync(FIL *fp) {
    (void)fp;
    return FR_OK;
}

static int short_read = -1;     // f_read of this slot gives nothing (a file that shrank)

FRESULT f_read(FIL *fp, void *buf, UINT btr, UINT *br) {
    if (fp->slot == short_read) {
        *br = 0;
        return FR_OK;
    }
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
    fno->fdate = card[i].fdate;
    fno->ftime = card[i].ftime;
    snprintf(fno->fname, sizeof(fno->fname), "%s", strrchr(card[i].path, '/') + 1);
    return FR_OK;
}

// Directories: the slots whose parent is the directory, in slot order. The DIR holds where the
// next one is looked for; the directory's path is kept here by DIR.
#define OPEN_DIRS 20
static struct {
    const DIR *dp;
    char path[CARD_PATH];
} open_dirs[OPEN_DIRS];
static int dirs_open, dirs_open_max;
static int readdir_fail_at = -1;   // f_readdir fails when it reaches this slot (-1: never)

FRESULT f_opendir(DIR *dp, const char *path) {
    int i = card_find(path);
    if (strcmp(path, "/") && (i < 0 || !card[i].dir))
        return FR_NO_PATH;
    for (int k = 0; k < OPEN_DIRS; k++)
        if (!open_dirs[k].dp) {
            open_dirs[k].dp = dp;
            snprintf(open_dirs[k].path, sizeof(open_dirs[k].path), "%s", path);
            dp->next = 0;
            dirs_open++;
            if (dirs_open > dirs_open_max)
                dirs_open_max = dirs_open;
            return FR_OK;
        }
    return FR_TOO_MANY_OPEN_FILES;
}

FRESULT f_readdir(DIR *dp, FILINFO *fno) {
    int k = 0;
    while (k < OPEN_DIRS && open_dirs[k].dp != dp)
        k++;
    CHECK(k < OPEN_DIRS);
    if (k == OPEN_DIRS)
        return FR_INVALID_OBJECT;
    const char *dir = open_dirs[k].path;
    size_t dl = strcmp(dir, "/") ? strlen(dir) : 0;
    for (; dp->next < CARD_FILES; dp->next++) {
        const char *p = card[dp->next].path;
        if (!p[0] || strncasecmp(p, dir, dl) || p[dl] != '/' || strchr(p + dl + 1, '/'))
            continue;
        if (dp->next == readdir_fail_at)
            return FR_DISK_ERR;
        f_stat(p, fno);
        dp->next++;
        return FR_OK;
    }
    fno->fname[0] = '\0';
    return FR_OK;
}

FRESULT f_closedir(DIR *dp) {
    for (int k = 0; k < OPEN_DIRS; k++)
        if (open_dirs[k].dp == dp) {
            open_dirs[k].dp = NULL;
            dirs_open--;
            return FR_OK;
        }
    CHECK(!"closedir of a directory not open");
    return FR_INVALID_OBJECT;
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

FRESULT f_mkdir(const char *path) {
    if (card_find(path) >= 0)
        return FR_EXIST;
    if (!parent_ok(path))
        return FR_NO_PATH;
    return card_add(path, true, "", 0) >= 0 ? FR_OK : FR_DENIED;
}

FRESULT f_utime(const char *path, const FILINFO *fno) {
    int i = card_find(path);
    if (i < 0)
        return FR_NO_FILE;
    card[i].fdate = fno->fdate;
    card[i].ftime = fno->ftime;
    return FR_OK;
}

// files.c's other commands aren't run here.
FRESULT f_lseek(FIL *fp, FSIZE_t ofs) { fp->pos = ofs; return FR_OK; }

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

bool sdcard_exfat(void) {
    return exfat;
}

static size_t heap_free_now;

size_t heap_free(void) {
    return heap_free_now;
}

// --- a shell ------------------------------------------------------------------------------------

static char *out;
static size_t out_len, out_cap;
static int write_space;         // what shell_write_space() says
static int cancel_after;        // shell_run_job cancels the job after this many steps (0: never)
static int steps;
static bool paced;              // shell_write must fit write_space (tf's lines)

void shell_write(shell_t *s, const char *buf, size_t len) {
    (void)s;
    if (paced)
        CHECK(len < (size_t)write_space);
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
    bool keep = paced;
    paced = false;
    if (n > 0)
        shell_write(sh, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    paced = keep;
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

// Run a line: split at spaces (the names here have none), then tar_commands' handler.
static void run(const char *line) {
    char buf[256];
    char *argv[16];
    int argc = 0;
    snprintf(buf, sizeof(buf), "%s", line);
    for (char *w = strtok(buf, " "); w && argc < 15; w = strtok(NULL, " "))
        argv[argc++] = w;
    argv[argc] = NULL;
    out_clear();
    CHECK(!strcmp(argv[0], tar_commands[0].name));
    tar_commands[0].runv(&sh, argc, argv);
}

static void setup(void) {
    card_clear();
    heap_free_now = 300 * 1024;
    write_space = 4096;
    paced = false;
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

#define CHECK_NOT(text) do {                                                                \
        CHECK(!out || !strstr(out, (text)));                                                \
        if (out && strstr(out, (text)))                                                     \
            fprintf(stderr, "  output: \"%s\"\n  not wanted in it: \"%s\"\n", out, (text)); \
    } while (0)

static void check_released(void) {
    CHECK_EQ(heap_tag_used(HEAP_FILES), 0);
    CHECK_EQ(heap_tag_used(HEAP_ZLIB), 0);
    CHECK(!files_busy());
    CHECK(bigjob_running() == NULL);
    CHECK_EQ(dirs_open, 0);
}

static char deep[256];          // dir/d<88 x>/f<48 y>.txt
static char longname[256];      // long/l<150 z>.txt

static void names(void) {
    strcpy(deep, "dir/d");
    memset(deep + 5, 'x', 88);
    strcpy(deep + 93, "/f");
    memset(deep + 95, 'y', 48);
    strcpy(deep + 143, ".txt");
    strcpy(longname, "long/l");
    memset(longname + 6, 'z', 150);
    strcpy(longname + 156, ".txt");
}

// The tree of make-archive-fixtures.sh under `base` ("" or "/out"), and its dates.
static void check_tree(const char *base, bool with_long) {
    char p[400];
    size_t n;
    uint8_t *text = fixture("text.txt", &n);
    snprintf(p, sizeof(p), "%s/dir/lines.txt", base);
    CHECK(card_holds(p, text, 10000));
    int i = card_exact(p);
    // 2026-03-04 05:06:07 UTC: FAT keeps seconds / 2.
    CHECK(i >= 0 && card[i].fdate == ((2026 - 1980) << 9 | 3 << 5 | 4));
    CHECK(i >= 0 && card[i].ftime == (5 << 11 | 6 << 5 | 3));
    uint8_t *rnd = fixture("random.bin", &n);
    snprintf(p, sizeof(p), "%s/dir/random.bin", base);
    CHECK(card_holds(p, rnd, 3000));
    snprintf(p, sizeof(p), "%s/dir/sub/empty", base);
    CHECK(card_holds(p, "", 0));
    snprintf(p, sizeof(p), "%s/dir/sub", base);
    CHECK(card_dir(p));
    i = card_exact(p);
    CHECK(i >= 0 && card[i].fdate == ((2026 - 1980) << 9 | 3 << 5 | 4));
    snprintf(p, sizeof(p), "%s/%s", base, deep);
    CHECK(card_text(p, "deep\n"));
    if (with_long) {
        snprintf(p, sizeof(p), "%s/%s", base, longname);
        CHECK(card_text(p, "long\n"));
    }
    CHECK(!card_has_part());
}

// --- tests --------------------------------------------------------------------------------------

TEST(the_table_follows_the_rule) {
    const shell_cmd_t *const tables[] = {tar_commands, NULL};
    CHECK(shell_tables_check(tables) == NULL);
}

TEST(xf_each_format) {
    static const struct {
        const char *file;
        bool with_long;
        const char *summary;
    } cases[] = {
        {"ustar.tar", false, "4 files, 3 directories, 0 skipped\n"},
        {"ustar.tgz", false, "4 files, 3 directories, 0 skipped\n"},
        {"gnu.tar", true, "5 files, 4 directories, 0 skipped\n"},
        {"gnu.tar.gz", true, "5 files, 4 directories, 0 skipped\n"},
        {"pax.tar", true, "5 files, 4 directories, 0 skipped\n"},
        {"bsd-pax.tar", true, "5 files, 4 directories, 0 skipped\n"},
        {"bsd-ustar.tar", false, "4 files, 3 directories, 0 skipped\n"},
    };
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        char a[64], line[128];
        setup();
        snprintf(a, sizeof(a), "/%s", cases[k].file);
        snprintf(line, sizeof(line), "tar/%s", cases[k].file);
        card_put(a, line);
        card_add("/out", true, "", 0);
        snprintf(line, sizeof(line), "tar xf %s -C /out", a);
        run(line);
        CHECK_HAS(cases[k].summary);
        CHECK_NOT("stopped");
        check_tree("/out", cases[k].with_long);
        CHECK(card_find(a) >= 0);               // the archive is kept
        CHECK(steps > 1);
        check_released();
    }
}

TEST(xf_v7_b256_signed_global) {
    setup();
    card_put("/v7.tar", "tar/v7.tar");
    card_put("/b.tar", "tar/b256.tar");
    card_put("/s.tar", "tar/signed.tar");
    card_put("/g.tar", "tar/global.tar");
    card_put("/o.tar", "tar/old.tar");
    run("tar xf v7.tar");
    CHECK_HAS("3 files, 2 directories, 0 skipped\n");    // dir/ made for dir/lines.txt
    CHECK(card_dir("/dir/sub"));
    CHECK(card_holds("/dir/sub/empty", "", 0));
    run("tar -xvf b.tar");
    CHECK_HAS("b256.txt\n1 files, 0 directories, 0 skipped\n");
    CHECK(card_text("/b256.txt", "base-256 size\n"));
    run("tar xzf s.tar");
    CHECK(card_text("/caf\xc3\xa9.txt", "signed\n"));
    run("tar xf g.tar");
    CHECK(card_find("/ignored.txt") < 0);
    CHECK(card_dir("/g"));
    run("tar xf o.tar");                    // 1901: FAT can't hold it; extraction time kept
    int i = card_exact("/old.txt");
    CHECK(i >= 0 && card[i].fdate == NOW_DATE);
    check_released();
}

TEST(tf_lists_and_says_what_xf_would_skip) {
    setup();
    card_put("/u.tar", "tar/ustar.tar");
    card_put("/u.tgz", "tar/ustar.tgz");
    card_put("/l.tar", "tar/links.tar");
    card_put("/e.tar", "tar/evil.tar");
    run("tar tf u.tar");
    CHECK_HAS("d            0  2026-03-04 05:06 UTC  dir/\n");
    CHECK_HAS("-        10000  2026-03-04 05:06 UTC  dir/lines.txt\n");
    CHECK_HAS("-            0  2026-03-04 05:06 UTC  dir/sub/empty\n");
    CHECK_NOT("files");
    char *plain = strdup(out);
    run("tar tf u.tgz");
    CHECK(!strcmp(out, plain));
    free(plain);
    run("tar tf l.tar");
    CHECK_HAS("p            0  2026-03-04 05:06 UTC  dir/fifo  (FIFO skipped)\n");
    CHECK_HAS("l            0  2026-03-04 05:06 UTC  dir/sym -> lines.txt  "
              "(link skipped (FAT has no links))\n");
    CHECK_HAS("h            0  2026-03-04 05:06 UTC  dir/lines.txt -> dir/hard  (link skipped");
    run("tar tf e.tar");
    CHECK_HAS("  ../evil.txt  (unsafe path)\n");
    CHECK_HAS("  /abs.txt\n");
    CHECK_HAS("  ctl?.txt  (not a valid FAT name)\n");
    CHECK_HAS("  cdev  (character device skipped)\n");
    CHECK_HAS("  bdev  (block device skipped)\n");
    CHECK_HAS("/../../long-up.txt  (unsafe path)\n");
    CHECK(card_find("/abs.txt") < 0);       // tf creates nothing
    check_released();
}

TEST(tf_is_paced_by_the_terminal) {
    setup();
    card_put("/g.tar", "tar/gnu.tar");
    write_space = 2048;
    run("tar tf g.tar");
    char *whole = strdup(out);
    write_space = 200;                      // a line at a time; long names wait for more room
    tick = 20000;                           // a piece of work a step
    paced = true;
    run("tar tf g.tar");
    paced = false;
    CHECK(steps > 9);
    write_space = 400;
    paced = true;
    run("tar tf g.tar");
    paced = false;
    CHECK(!strcmp(out, whole));
    free(whole);
    check_released();
}

TEST(xf_safety_rules) {
    setup();
    card_put("/e.tar", "tar/evil.tar");
    card_add("/t", true, "", 0);
    run("tar xf /e.tar -C /t");
    CHECK_HAS("tar: ../evil.txt: unsafe path\n");
    CHECK_HAS("tar: removing leading / from names\n");
    CHECK_HAS("tar: ok/../../up.txt: unsafe path\n");
    CHECK_HAS("tar: bad:name.txt: not a valid FAT name\n");
    CHECK_HAS("tar: trail.: not a valid FAT name\n");
    CHECK_HAS("tar: ctl?.txt: not a valid FAT name\n");
    CHECK_HAS("tar: back\\..\\slash.txt: not a valid FAT name\n");
    CHECK_HAS("tar: README.TXT: same name as Readme.txt on FAT\n");
    CHECK_HAS("tar: hl: link skipped (FAT has no links)\n");
    CHECK_HAS("tar: sl: link skipped (FAT has no links)\n");
    CHECK_HAS("tar: cdev: character device skipped\n");
    CHECK_HAS("tar: bdev: block device skipped\n");
    CHECK_HAS("tar: fifo: FIFO skipped\n");
    CHECK_HAS("/../../long-up.txt: unsafe path\n");
    CHECK_HAS("6 files, 3 directories, 13 skipped\n");    // same.txt twice
    CHECK(card_text("/t/abs.txt", "abs\n"));
    CHECK(card_text("/t/ok/x/y.txt", "y\n"));
    CHECK(card_text("/t/Readme.txt", "first\n"));
    CHECK(card_text("/t/same.txt", "two\n"));          // the later one wins
    CHECK(card_text("/t/certs/evil.pem", "not a certificate\n"));   // /t/certs isn't /certs
    CHECK(card_find("/evil.txt") < 0 && card_find("/up.txt") < 0 && card_find("/t/hl") < 0);
    CHECK(card_find("/t/sl") < 0 && card_find("/t/cdev") < 0 && card_find("/long-up.txt") < 0);
    CHECK(card_find("/t/a") < 0);                       // nothing made for a refused name
    CHECK(!card_has_part());
    // Only once: "removing leading /".
    const char *first = strstr(out, "removing leading /");
    CHECK(first && !strstr(first + 1, "removing leading /"));
    check_released();

    setup();                                // extracted at / over SSH: /certs is protected
    card_put("/e.tar", "tar/evil.tar");
    sh.io = &io_ssh;
    run("tar xf e.tar");
    CHECK_HAS("tar: certs/evil.pem: not allowed to change this path\n");
    CHECK(card_find("/certs") < 0);
    sh.io = &io_console;
    run("tar xf e.tar -f");                 // the console may
    CHECK(card_text("/certs/evil.pem", "not a certificate\n"));
    check_released();
}

TEST(xf_pax_and_long_names) {
    setup();
    card_put("/p.tar", "tar/pax-evil.tar");
    card_put("/t.tar", "tar/toolong.tar");
    card_put("/b.tar", "tar/paxbig.tar");
    run("tar xf p.tar");
    CHECK_HAS("tar: ../pax-up.txt: unsafe path\n");
    CHECK_HAS("tar: removing leading / from names\n");
    CHECK_HAS("tar: sl: link skipped");
    CHECK_HAS("2 files, 1 directories, 2 skipped\n");
    CHECK(card_text("/pax-ok.txt", "ok\n"));
    run("tar xf t.tar");
    CHECK_HAS(".txt: name too long\n");
    CHECK_HAS("1 files, 0 directories, 2 skipped\n");
    CHECK(card_text("/ok.txt", "ok\n"));
    run("tar xf b.tar -f");
    CHECK_HAS("tar: big-pax.txt: pax header too large\n");
    CHECK(card_find("/big-pax.txt") < 0);
    check_released();
}

TEST(existing_files_need_f) {
    setup();
    card_put("/u.tar", "tar/ustar.tar");
    card_add("/dir", true, "", 0);
    card_add("/dir/lines.txt", false, "old", 3);
    card_add("/dir/random.bin", true, "", 0);           // a directory where a file goes
    run("tar xf u.tar");
    CHECK_HAS("tar: dir/lines.txt: exists (-f replaces it)\n");
    CHECK_HAS("tar: dir/random.bin: is a directory\n");
    CHECK_HAS("2 files, 2 directories, 2 skipped\n");
    CHECK(card_text("/dir/lines.txt", "old"));
    run("tar xf u.tar -f");
    CHECK_HAS("3 files, 0 directories, 1 skipped\n");
    size_t n;
    uint8_t *text = fixture("text.txt", &n);
    CHECK(card_holds("/dir/lines.txt", text, 10000));
    CHECK(card_find("/dir/lines.txt.old") < 0);
    // A file where a directory goes.
    setup();
    card_put("/u.tar", "tar/ustar.tar");
    card_add("/dir", false, "file", 4);
    run("tar xf u.tar -f");
    CHECK_HAS("tar: dir/: a file is in the way\n");
    CHECK_HAS("tar: dir/lines.txt: a file is in the way of its directory\n");
    CHECK_HAS("0 files, 0 directories, 7 skipped\n");
    CHECK(card_text("/dir", "file"));
    // The archive itself, among the entries.
    setup();
    card_add("/out", true, "", 0);
    card_add("/out/dir", true, "", 0);
    card_put("/out/dir/lines.txt", "tar/ustar.tar");
    run("tar xf /out/dir/lines.txt -C /out -f");
    CHECK_HAS("tar: dir/lines.txt: is the archive itself\n");
    CHECK(card_holds("/out/dir/random.bin", fixture("random.bin", &n), 3000));
    check_released();
}

TEST(stops_leave_no_part) {
    setup();
    card_put("/g.tgz", "tar/gnu.tar.gz");
    tick = 20000;                           // a piece of work a step
    cancel_after = 6;
    run("tar xf g.tgz");
    CHECK_HAS("stopped: Ctrl-C\n");
    CHECK_HAS(" files, ");
    CHECK(!card_has_part());
    check_released();

    setup();
    card_put("/b.tar", "tar/badsum.tar");
    run("tar xf b.tar");
    CHECK_HAS("0 files, 2 directories, 0 skipped\nstopped: bad header at 1024\n");
    check_released();

    setup();
    card_put("/c.tar", "tar/cut.tar");
    run("tar xf c.tar");
    CHECK_HAS("stopped: archive ends early\n");
    CHECK(!card_has_part());
    CHECK(card_find("/dir/lines.txt") < 0);             // cut in its data: not installed
    check_released();

    setup();
    card_put("/n.tar", "tar/noend.tar");
    run("tar xf n.tar");
    CHECK_HAS("4 files, 3 directories, 0 skipped\nstopped: archive ends early\n");
    check_released();

    setup();
    card_put("/c.tgz", "tar/cut.tgz");
    run("tar tf c.tgz");
    CHECK_HAS("stopped: ");
    CHECK_NOT("files,");
    check_released();

    setup();                                // a full card
    card_put("/u.tar", "tar/ustar.tar");
    card_room = card_used() + 5000;
    run("tar xf u.tar");
    CHECK_HAS("stopped: card full\n");
    CHECK(!card_has_part());
    check_released();
}

TEST(fat32_size_limit) {
    // A header for a 5 GB file (base-256 size), and no data: refused before anything is written.
    static uint8_t a[2048];
    memset(a, 0, sizeof(a));
    strcpy((char *)a, "huge.bin");
    a[124] = 0x80;
    uint64_t size = 5ull << 30;
    for (int i = 0; i < 8; i++)
        a[135 - i] = (uint8_t)(size >> (8 * i));
    a[156] = '0';
    memcpy(a + 257, "ustar\0" "00", 8);
    memset(a + 148, ' ', 8);
    unsigned s = 0;
    for (int i = 0; i < 512; i++)
        s += a[i];
    snprintf((char *)a + 148, 8, "%06o", s);
    setup();
    card_add("/h.tar", false, a, sizeof(a));
    run("tar xf h.tar");
    CHECK_HAS("tar: huge.bin: too large for FAT32\n");
    CHECK_HAS("stopped: archive ends early\n");
    CHECK(card_find("/huge.bin") < 0 && !card_has_part());
    exfat = true;
    run("tar xf h.tar");
    CHECK_NOT("FAT32");
    CHECK(!card_has_part());
    check_released();
}

TEST(usage_dirs_memory_one_at_a_time) {
    setup();
    card_put("/u.tar", "tar/ustar.tar");
    static const char *const bad[] = {"tar", "tar tf", "tar t u.tar", "tar xtf u.tar",
                                      "tar cf u.tar", "tar ctf u.tar x", "tar cf u.tar -C", "tar tf u.tar -f", "tar xf u.tar -C",
                                      "tar xf u.tar -q", "tar xf u.tar -C /a -C /b"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        run(bad[i]);
        CHECK_HAS("usage: tar tf <archive>\n");
    }
    run("tar xf u.tar -C /nowhere");
    CHECK_HAS("tar: /nowhere: no such directory\n");
    run("tar xf missing.tar");
    CHECK_HAS("tar: missing.tar: no such file\n");
    heap_free_now = 70 * 1024;
    run("tar tf u.tar");
    CHECK_HAS("tar: not enough memory: needs ");
    card_put("/u.tgz", "tar/ustar.tgz");
    heap_free_now = 100 * 1024;             // enough for plain, not with gzip's 40 KB
    run("tar tf u.tgz");
    CHECK_HAS("tar: not enough memory: gzip needs 40 KB more");
    heap_free_now = 300 * 1024;
    CHECK(bigjob_try_begin("gunzip") == NULL);
    run("tar tf u.tar");
    CHECK_HAS("busy: gunzip is running in another shell\n");
    bigjob_end();
    strcpy(sh.cwd, "/sub");
    card_add("/sub", true, "", 0);
    card_put("/sub/x.tar", "tar/b256.tar");
    run("tar -xf x.tar -C .");
    CHECK(card_text("/sub/b256.txt", "base-256 size\n"));
    check_released();
}

// --- creating ------------------------------------------------------------------------------------

#define C_DATE ((2026 - 1980) << 9 | 3 << 5 | 4)   // 2026-03-04 05:06:06 UTC
#define C_TIME (5 << 11 | 6 << 5 | 3)
#define C_T0   1772600766

// A file or directory as a finished one is: dated (a file without a date is being written).
static int card_mk(const char *path, bool dir, const void *data, size_t len) {
    int i = card_add(path, dir, data, len);
    if (i >= 0) {
        card[i].fdate = C_DATE;
        card[i].ftime = C_TIME;
    }
    return i;
}

static uint8_t c_random[10000];
static char c_long[300];        // t/<120 L>/<110 M>.txt: a pax name
static char c_deep[300];        // t/deep/d1/.../d12/f.txt

// The tree under /t, and the entries a tar made of `t` from / holds (names from "t").
static size_t c_tree(tt_entry_t *e) {
    for (size_t i = 0; i < sizeof(c_random); i++)
        c_random[i] = (uint8_t)(i * 7919 >> 3 ^ i);
    char *p = c_long + sprintf(c_long, "t/");
    memset(p, 'L', 120);
    p += 120;
    *p++ = '/';
    memset(p, 'M', 110);
    strcpy(p + 110, ".txt");
    p = c_deep + sprintf(c_deep, "t/deep");
    size_t n = 0;
    char path[CARD_PATH];
    e[n++] = (tt_entry_t){"t", true, NULL, 0, 0};
    card_mk("/t", true, "", 0);
    e[n++] = (tt_entry_t){"t/a.txt", false, (const uint8_t *)"hello\n", 6, C_T0};
    card_mk("/t/a.txt", false, "hello\n", 6);
    e[n++] = (tt_entry_t){"t/empty", false, (const uint8_t *)"", 0, C_T0};
    card_mk("/t/empty", false, "", 0);
    e[n++] = (tt_entry_t){"t/emptydir", true, NULL, 0, 0};
    card_mk("/t/emptydir", true, "", 0);
    e[n++] = (tt_entry_t){"t/rnd.bin", false, c_random, sizeof(c_random), C_T0};
    card_mk("/t/rnd.bin", false, c_random, sizeof(c_random));
    e[n++] = (tt_entry_t){"t/caf\xc3\xa9.txt", false, (const uint8_t *)"caf\xc3\xa9\n", 6, C_T0};
    card_mk("/t/caf\xc3\xa9.txt", false, "caf\xc3\xa9\n", 6);
    static char dirs[13][300];
    for (int d = 0; d <= 12; d++) {
        if (d)
            p += sprintf(p, "/d%d", d);
        strcpy(dirs[d], c_deep);
        e[n++] = (tt_entry_t){dirs[d], true, NULL, 0, 0};
        snprintf(path, sizeof(path), "/%s", c_deep);
        card_mk(path, true, "", 0);
    }
    strcpy(p, "/f.txt");
    e[n++] = (tt_entry_t){c_deep, false, (const uint8_t *)"deep\n", 5, C_T0};
    snprintf(path, sizeof(path), "/%s", c_deep);
    card_mk(path, false, "deep\n", 5);
    static char long_dir[300];
    snprintf(long_dir, sizeof(long_dir), "%.122s", c_long);
    e[n++] = (tt_entry_t){long_dir, true, NULL, 0, 0};
    snprintf(path, sizeof(path), "/%s", long_dir);
    card_mk(path, true, "", 0);
    e[n++] = (tt_entry_t){c_long, false, (const uint8_t *)"long\n", 5, C_T0};
    snprintf(path, sizeof(path), "/%s", c_long);
    card_mk(path, false, "long\n", 5);
    return n;
}

// Every file and directory under `from` is under `to` too, with the same contents and dates.
static bool same_tree(const char *from, const char *to) {
    size_t fl = strlen(from);
    bool ok = true;
    for (int i = 0; i < CARD_FILES && ok; i++) {
        if (!card[i].path[0] || strncmp(card[i].path, from, fl) ||
            (card[i].path[fl] && card[i].path[fl] != '/'))
            continue;
        char p[CARD_PATH];
        snprintf(p, sizeof(p), "%s%s", to, card[i].path + fl);
        int k = card_exact(p);
        ok = k >= 0 && card[k].dir == card[i].dir && card[k].len == card[i].len &&
             (!card[i].len || !memcmp(card[k].data, card[i].data, card[i].len)) &&
             card[k].fdate == card[i].fdate && card[k].ftime == card[i].ftime;
        if (!ok)
            fprintf(stderr, "  %s: not the same as %s\n", p, card[i].path);
    }
    return ok;
}

static const uint8_t *card_data(const char *path, size_t *len) {
    int i = card_exact(path);
    *len = i >= 0 ? card[i].len : 0;
    return i >= 0 ? card[i].data : NULL;
}

TEST(cf_round_trip) {
    setup();
    static tt_entry_t e[32];
    size_t n = c_tree(e);
    run("tar cf a.tar t");
    char want[96];
    snprintf(want, sizeof(want), "a.tar: %zu files, %zu directories, ", (size_t)6, n - 6);
    CHECK_HAS(want);
    CHECK_HAS(" bytes (0 skipped)\n");
    size_t len;
    const uint8_t *a = card_data("/a.tar", &len);
    CHECK(a && len % 10240 == 0 && len >= 10240);
    snprintf(want, sizeof(want), ", %zu bytes (", len);
    CHECK_HAS(want);
    CHECK(!card_has_part());
    check_released();
    CHECK(tt_check("tar", a, len, e, n));
    CHECK(tt_check("bsdtar", a, len, e, n));
    // Ours, back: the same files with the same dates.
    card_mk("/out", true, "", 0);
    run("tar xf a.tar -C /out");
    CHECK_HAS("6 files, 16 directories, 0 skipped\n");
    CHECK(same_tree("/t", "/out/t"));
    check_released();
}

TEST(czf_round_trip) {
    setup();
    static tt_entry_t e[32];
    size_t n = c_tree(e);
    run("tar -czvf /a.tgz t");
    CHECK_HAS("t/\nt/a.txt\n");
    CHECK_HAS("a.tgz: 6 files, 16 directories, ");
    size_t len;
    const uint8_t *a = card_data("/a.tgz", &len);
    CHECK(a && len > 2 && a[0] == 0x1f && a[1] == 0x8b && len < 10240);
    check_released();
    CHECK(tt_check("tar", a, len, e, n));
    CHECK(tt_check("bsdtar", a, len, e, n));
    card_mk("/out", true, "", 0);
    run("tar xzf a.tgz -C /out");
    CHECK_HAS("6 files, 16 directories, 0 skipped\n");
    CHECK(same_tree("/t", "/out/t"));
    // Not enough memory for gzip's encoder, enough for plain.
    heap_free_now = 100 * 1024;
    run("tar czf b.tgz t");
    CHECK_HAS("tar: not enough memory: needs ");
    run("tar cf b.tar t");
    CHECK_HAS("b.tar: 6 files");
    check_released();
}

TEST(cf_names_as_given) {
    setup();
    static tt_entry_t e[32];
    c_tree(e);
    // -C: names relative to it; v lists them.
    run("tar cvf /b.tar -C /t a.txt emptydir");
    CHECK_HAS("a.txt\nemptydir/\n/b.tar: 1 files, 1 directories, ");
    size_t len;
    const uint8_t *a = card_data("/b.tar", &len);
    const tt_entry_t b[] = {{"a.txt", false, (const uint8_t *)"hello\n", 6, C_T0},
                            {"emptydir", true, NULL, 0, 0}};
    CHECK(tt_check("tar", a, len, b, 2));
    // A leading '/' is removed, and said once; "." is -C's directory, with no entry of its own.
    strcpy(sh.cwd, "/t");
    run("tar cf /c.tar /t/a.txt /t/empty");
    CHECK_HAS("tar: removing leading / from names\n/c.tar: 2 files, 0 directories, ");
    a = card_data("/c.tar", &len);
    const tt_entry_t c[] = {{"t/a.txt", false, (const uint8_t *)"hello\n", 6, C_T0},
                            {"t/empty", false, (const uint8_t *)"", 0, C_T0}};
    CHECK(tt_check("tar", a, len, c, 2));
    run("tar cf /d.tar -C deep/d1/d2/d3/d4/d5/d6/d7/d8/d9/d10/d11 .");
    CHECK_HAS("d.tar: 1 files, 1 directories, ");
    a = card_data("/d.tar", &len);
    const tt_entry_t d[] = {{"d12", true, NULL, 0, 0},
                            {"d12/f.txt", false, (const uint8_t *)"deep\n", 5, C_T0}};
    CHECK(tt_check("tar", a, len, d, 2));
    // ".." is refused, as tar xf would refuse it; a missing path is said; the rest goes in.
    run("tar cf /e.tar ../t/a.txt nope a.txt");
    CHECK_HAS("tar: ../t/a.txt: unsafe path\n");
    CHECK_HAS("tar: nope: no such file or directory\n");
    CHECK_HAS("e.tar: 1 files, 0 directories, ");
    CHECK_HAS(" (2 skipped)\n");
    // The whole card from the root.
    strcpy(sh.cwd, "/");
    run("tar cf /t/emptydir/all.tar /");
    CHECK_HAS("tar: t/emptydir/all.tar.part: the archive itself, skipped\n");
    CHECK_HAS("/t/emptydir/all.tar: 10 files, 16 directories, ");
    check_released();
}

TEST(cf_skips) {
    setup();
    static tt_entry_t e[32];
    size_t n = c_tree(e);
    // Being written: no date yet, or locked by FatFs while open for writing; the archive itself
    // (an old one replaced with -f, and the .part); a directory that can't be read on.
    card_add("/t/new.log", false, "growing", 7);
    int i = card_mk("/t/open.log", false, "open", 4);
    card[i].locked = true;
    card_mk("/t/self.tar", false, "old", 3);
    run("tar cf t/self.tar t");
    CHECK_HAS("tar: t/self.tar: already exists (-f replaces it)\n");
    run("tar cf t/self.tar t -f");
    CHECK_HAS("tar: t/new.log: being written, skipped\n");
    CHECK_HAS("tar: t/open.log: being written, skipped\n");
    CHECK_HAS("tar: t/self.tar: the archive itself, skipped\n");
    CHECK_HAS("tar: t/self.tar.part: the archive itself, skipped\n");
    CHECK_HAS("self.tar: 6 files, ");
    CHECK_HAS(" (4 skipped)\n");
    size_t len;
    const uint8_t *a = card_data("/t/self.tar", &len);
    CHECK(len > 3);
    CHECK(tt_check("tar", a, len, e, n));
    CHECK(!card_has_part());
    check_released();
    // A directory read error: said, the rest archived.
    readdir_fail_at = card_find("/t/rnd.bin");
    run("tar cf x.tar t");
    readdir_fail_at = -1;
    CHECK_HAS("tar: t: error\n");
    CHECK_HAS("x.tar: 2 files, 2 directories, 10240 bytes (1 skipped)\n");
    check_released();
    // Nothing to archive: nothing made.
    run("tar cf y.tar nope t/new.log");
    CHECK_HAS("tar: nothing to archive; y.tar not made\n");
    CHECK(card_find("/y.tar") < 0 && !card_has_part());
    check_released();
}

TEST(cf_depth_and_name_limits) {
    setup();
    char path[CARD_PATH] = "/z";
    card_mk(path, true, "", 0);
    for (int d = 1; d <= 17; d++) {
        sprintf(path + strlen(path), "/l%d", d);
        card_mk(path, true, "", 0);
    }
    strcat(path, "/f");
    card_mk(path, false, "x", 1);
    run("tar cf a.tar z");
    CHECK_HAS("tar: z/l1/l2/l3/l4/l5/l6/l7/l8/l9/l10/l11/l12/l13/l14/l15/l16: more than 16 "
              "directories deep, skipped\n");
    CHECK_HAS("a.tar: 0 files, 16 directories, ");
    CHECK_EQ(dirs_open, 0);
    CHECK_EQ(dirs_open_max, 4);             // FatFs's lock table is shared: 4 at most
    // Levels closed to keep to that are opened again and read on past what was taken.
    card_mk("/w", true, "", 0);
    char want_path[64] = "/w";
    for (const char *d = "abcde"; *d; d++) {
        sprintf(want_path + strlen(want_path), "/%c", *d);
        card_mk(want_path, true, "", 0);
    }
    card_mk("/w/a/b/c/d/e/f.txt", false, "f", 1);
    card_mk("/w/a/b/c/d/after", false, "4", 1);
    card_mk("/w/a/after", false, "1", 1);
    card_mk("/w/after", false, "0", 1);
    run("tar cf w.tar w");
    CHECK_HAS("w.tar: 4 files, 6 directories, ");
    size_t len;
    const uint8_t *a = card_data("/w.tar", &len);
    const tt_entry_t w[] = {{"w", true, NULL, 0, 0},
                            {"w/a", true, NULL, 0, 0},
                            {"w/a/b", true, NULL, 0, 0},
                            {"w/a/b/c", true, NULL, 0, 0},
                            {"w/a/b/c/d", true, NULL, 0, 0},
                            {"w/a/b/c/d/e", true, NULL, 0, 0},
                            {"w/a/b/c/d/e/f.txt", false, (const uint8_t *)"f", 1, C_T0},
                            {"w/a/b/c/d/after", false, (const uint8_t *)"4", 1, C_T0},
                            {"w/a/after", false, (const uint8_t *)"1", 1, C_T0},
                            {"w/after", false, (const uint8_t *)"0", 1, C_T0}};
    CHECK(tt_check("tar", a, len, w, sizeof(w) / sizeof(w[0])));
    // A name longer than the reader takes: skipped. (Components of 250: the third makes 764.)
    char name[CARD_PATH] = "/n";
    card_mk(name, true, "", 0);
    for (int d = 0; d < 3; d++) {
        strcat(name, "/");
        memset(name + strlen(name), 'a' + d, 250);
        card_mk(name, true, "", 0);
    }
    strcat(name, "/ffffffffffff");
    card_mk(name, false, "x", 1);
    run("tar cf b.tar n");
    CHECK_HAS("tar: n/aaaa");
    CHECK_HAS("/ffffffffff: name too long, skipped\n");
    CHECK_HAS("b.tar: 0 files, 4 directories, ");
    check_released();
}

TEST(cf_stops) {
    setup();
    static tt_entry_t e[32];
    c_tree(e);
    // A file that shrinks while read: zeros to its header's size.
    short_read = card_find("/t/rnd.bin");
    run("tar cf a.tar t/rnd.bin");
    short_read = -1;
    CHECK_HAS("tar: t/rnd.bin: shrank while read\ntar: t/rnd.bin: padded with zeros to its size\n");
    size_t len;
    const uint8_t *a = card_data("/a.tar", &len);
    static const uint8_t zeros10k[10000];
    const tt_entry_t z[] = {{"t/rnd.bin", false, zeros10k, sizeof(zeros10k), C_T0}};
    CHECK(tt_check("tar", a, len, z, 1));
    // Ctrl-C, a full card: no .part, no archive.
    cancel_after = 3;
    tick = 30000;                           // a piece of work a step
    run("tar czf b.tgz t");
    tick = 1000;
    CHECK_HAS("stopped: Ctrl-C; b.tgz not made\n");
    CHECK(card_find("/b.tgz") < 0 && !card_has_part());
    check_released();
    cancel_after = 0;
    card_room = card_used() + 3000;
    run("tar cf b.tar t");
    CHECK_HAS("stopped: card full; b.tar not made\n");
    CHECK(card_find("/b.tar") < 0 && !card_has_part());
    check_released();
    card_room = 64u << 20;
    // Busy; protected paths; no -C directory; a directory in the archive's place.
    CHECK(bigjob_try_begin("gzip") == NULL);
    run("tar cf b.tar t");
    CHECK_HAS("busy: gzip is running in another shell\n");
    bigjob_end();
    sh.io = &io_ssh;
    run("tar cf /certs/x.tar t");
    CHECK_HAS("tar: /certs/x.tar: not allowed to change this path\n");
    sh.io = &io_console;
    run("tar cf b.tar -C /nowhere t");
    CHECK_HAS("tar: /nowhere: no such directory\n");
    run("tar cf t t");
    CHECK_HAS("tar: t: is a directory\n");
    check_released();
}

TEST_MAIN(
    names();
    RUN(the_table_follows_the_rule);
    RUN(xf_each_format);
    RUN(xf_v7_b256_signed_global);
    RUN(tf_lists_and_says_what_xf_would_skip);
    RUN(tf_is_paced_by_the_terminal);
    RUN(xf_safety_rules);
    RUN(xf_pax_and_long_names);
    RUN(existing_files_need_f);
    RUN(stops_leave_no_part);
    RUN(fat32_size_limit);
    RUN(usage_dirs_memory_one_at_a_time);
    RUN(cf_round_trip);
    RUN(czf_round_trip);
    RUN(cf_names_as_given);
    RUN(cf_skips);
    RUN(cf_depth_and_name_limits);
    RUN(cf_stops);
    card_clear();
    free(out);
)
