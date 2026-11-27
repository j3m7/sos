// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/ssh/sftp_fs.c, the SFTP server's file functions, with the real
// src/files/files.c behind them: when an upload's file is synced (files_sync_if_due(), #224),
// and that reads and errors behave as before. FatFs is stood in for by a card in memory
// (tests/stubs/ff.h) whose f_sync() records its calls, and the clock moves only when a test
// moves it.

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

#include "ff.h"
#include "f_util.h"
#include "pico/stdlib.h"

#include "files/files.h"
#include "files/sdcard.h"
#include "shell/shell.h"
#include "ssh/sftp_fs.h"
#include "test.h"

// --- a card in memory, behind FatFs's functions -------------------------------------------------

#define CARD_FILES 4

static struct {
    char path[64];              // "" for a free slot
    char data[256];
    size_t len;
} card[CARD_FILES];

static int nsyncs;              // f_sync() calls
static FRESULT sync_result;     // what f_sync() returns
static FRESULT write_result;    // what f_write() returns
static uint64_t now;            // the clock (µs); tests move it

uint64_t time_us_64(void) {
    return now;
}

static int card_find(const char *path) {
    for (int i = 0; i < CARD_FILES; i++)
        if (card[i].path[0] && !strcmp(card[i].path, path))
            return i;
    return -1;
}

static int card_add(const char *path, const char *data) {
    for (int i = 0; i < CARD_FILES; i++)
        if (!card[i].path[0]) {
            snprintf(card[i].path, sizeof(card[i].path), "%s", path);
            snprintf(card[i].data, sizeof(card[i].data), "%s", data);
            card[i].len = strlen(card[i].data);
            return i;
        }
    return -1;
}

FSIZE_t test_f_size(const FIL *fp) {
    return card[fp->slot].len;
}

FRESULT f_open(FIL *fp, const char *path, uint8_t mode) {
    int i = card_find(path);
    if (i < 0) {
        if (!(mode & (FA_CREATE_ALWAYS | FA_OPEN_ALWAYS | FA_CREATE_NEW)))
            return FR_NO_FILE;
        if ((i = card_add(path, "")) < 0)
            return FR_DENIED;
    } else if ((mode & FA_OPEN_APPEND) != FA_OPEN_APPEND && (mode & FA_CREATE_ALWAYS)) {
        card[i].len = 0;
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

FRESULT f_lseek(FIL *fp, FSIZE_t ofs) {
    if (ofs > sizeof(card[0].data))
        return FR_DENIED;
    fp->pos = ofs;
    return FR_OK;
}

FRESULT f_read(FIL *fp, void *buf, UINT btr, UINT *br) {
    if (!(fp->mode & FA_READ))
        return FR_DENIED;
    size_t left = card[fp->slot].len > fp->pos ? card[fp->slot].len - fp->pos : 0;
    *br = btr < left ? btr : (UINT)left;
    memcpy(buf, card[fp->slot].data + fp->pos, *br);
    fp->pos += *br;
    return FR_OK;
}

FRESULT f_write(FIL *fp, const void *buf, UINT btw, UINT *bw) {
    if (!(fp->mode & FA_WRITE))
        return FR_DENIED;
    if (write_result != FR_OK)
        return write_result;
    size_t room = sizeof(card[0].data) - fp->pos;
    *bw = btw < room ? btw : (UINT)room;
    memcpy(card[fp->slot].data + fp->pos, buf, *bw);
    fp->pos += *bw;
    if (fp->pos > card[fp->slot].len)
        card[fp->slot].len = fp->pos;
    return FR_OK;
}

FRESULT f_sync(FIL *fp) {
    (void)fp;
    nsyncs++;
    return sync_result;
}

// What files.c uses besides; SFTP never gets there.
FRESULT f_stat(const char *path, FILINFO *fno) { (void)path; (void)fno; return FR_NO_FILE; }
FRESULT f_opendir(DIR *dp, const char *path) { (void)dp; (void)path; return FR_NO_PATH; }
FRESULT f_readdir(DIR *dp, FILINFO *fno) { (void)dp; fno->fname[0] = '\0'; return FR_OK; }
FRESULT f_closedir(DIR *dp) { (void)dp; return FR_OK; }
FRESULT f_mkdir(const char *path) { (void)path; return FR_DENIED; }
FRESULT f_unlink(const char *path) { (void)path; return FR_DENIED; }
FRESULT f_rename(const char *path_old, const char *path_new) {
    (void)path_old;
    (void)path_new;
    return FR_DENIED;
}
const char *FRESULT_str(FRESULT i) { (void)i; return "error"; }
bool sdcard_mount(shell_t *sh) { (void)sh; return true; }
bool sdcard_mounted(void) { return true; }
void shell_write(shell_t *sh, const char *buf, size_t len) { (void)sh; (void)buf; (void)len; }
void shell_printf(shell_t *sh, const char *fmt, ...) { (void)sh; (void)fmt; }
int shell_write_space(shell_t *sh) { (void)sh; return 1024; }
void shell_run_job(shell_t *sh, shell_job_fn step, void *arg) { (void)sh; (void)step; (void)arg; }

// --- helpers -------------------------------------------------------------------------------------

#define SECOND 1000000u

static void setup(void) {
    memset(card, 0, sizeof(card));
    nsyncs = 0;
    sync_result = write_result = FR_OK;
    now = 5 * SECOND;
    card_add("/old", "hello");
}

static int put(int fd, const char *s, unsigned int at) {
    const unsigned int ofst[2] = {at, 0};
    return sftp_fs_pwrite(fd, (const unsigned char *)s, (int)strlen(s), ofst);
}

// --- tests ---------------------------------------------------------------------------------------

TEST(an_upload_is_synced_once_a_second_has_passed_and_not_before) {
    setup();
    int fd = sftp_fs_open("/up", FA_WRITE | FA_CREATE_ALWAYS);
    CHECK(fd >= 0);
    now += SECOND - 1;
    CHECK_EQ(put(fd, "abc", 0), 3);
    CHECK_EQ(nsyncs, 0);                    // not yet a second since the open
    now += 1;
    CHECK_EQ(put(fd, "def", 3), 3);
    CHECK_EQ(nsyncs, 1);                    // a second since the open
    CHECK_EQ(sftp_fs_close(fd), 0);
    CHECK_EQ(nsyncs, 1);                    // close writes the entry itself (f_close())
    CHECK_EQ(card[card_find("/up")].len, 6);
    CHECK(files_busy() == false);
}

TEST(writes_within_a_second_cause_no_sync) {
    setup();
    int fd = sftp_fs_open("/up", FA_WRITE | FA_CREATE_ALWAYS);
    CHECK(fd >= 0);
    for (unsigned int i = 0; i < 10; i++) {
        now += SECOND / 20;
        CHECK_EQ(put(fd, "x", i), 1);
    }
    CHECK_EQ(nsyncs, 0);
    // From the open's second on, once a second: the clock counts from the last sync.
    now += SECOND / 2;
    CHECK_EQ(put(fd, "y", 10), 1);
    CHECK_EQ(nsyncs, 1);
    now += SECOND / 2;
    CHECK_EQ(put(fd, "y", 11), 1);
    CHECK_EQ(nsyncs, 1);
    now += SECOND / 2;
    CHECK_EQ(put(fd, "y", 12), 1);
    CHECK_EQ(nsyncs, 2);
    CHECK_EQ(sftp_fs_close(fd), 0);
}

TEST(each_handle_keeps_its_own_clock) {
    setup();
    int a = sftp_fs_open("/a", FA_WRITE | FA_CREATE_ALWAYS);
    now += SECOND / 2;
    int b = sftp_fs_open("/b", FA_WRITE | FA_CREATE_ALWAYS);
    CHECK(a >= 0 && b >= 0 && a != b);
    now += SECOND / 2;
    CHECK_EQ(put(a, "a", 0), 1);
    CHECK_EQ(put(b, "b", 0), 1);
    CHECK_EQ(nsyncs, 1);                    // only a's second is up
    now += SECOND / 2;
    CHECK_EQ(put(b, "b", 1), 1);
    CHECK_EQ(nsyncs, 2);
    CHECK_EQ(sftp_fs_close(a), 0);
    CHECK_EQ(sftp_fs_close(b), 0);
}

TEST(a_handle_reused_after_close_starts_its_clock_at_the_open) {
    setup();
    int fd = sftp_fs_open("/a", FA_WRITE | FA_CREATE_ALWAYS);
    CHECK_EQ(sftp_fs_close(fd), 0);
    now += 10 * SECOND;
    int again = sftp_fs_open("/b", FA_WRITE | FA_CREATE_ALWAYS);
    CHECK_EQ(again, fd);
    CHECK_EQ(put(again, "b", 0), 1);
    CHECK_EQ(nsyncs, 0);                    // the old handle's time doesn't carry over
    CHECK_EQ(sftp_fs_close(again), 0);
}

TEST(a_failed_sync_is_reported_as_a_failed_write) {
    setup();
    int fd = sftp_fs_open("/up", FA_WRITE | FA_CREATE_ALWAYS);
    sync_result = FR_DISK_ERR;
    now += SECOND;
    CHECK_EQ(put(fd, "abc", 0), -1);
    CHECK_EQ(nsyncs, 1);
    sync_result = FR_OK;
    CHECK_EQ(put(fd, "abc", 0), 3);         // the next try within the second: no sync
    CHECK_EQ(nsyncs, 1);
    CHECK_EQ(sftp_fs_close(fd), 0);
}

TEST(a_failed_write_is_reported_and_not_synced) {
    setup();
    int fd = sftp_fs_open("/up", FA_WRITE | FA_CREATE_ALWAYS);
    write_result = FR_DISK_ERR;
    now += SECOND;
    CHECK_EQ(put(fd, "abc", 0), -1);
    CHECK_EQ(nsyncs, 0);
    CHECK_EQ(sftp_fs_close(fd), 0);
}

TEST(a_read_only_handle_never_syncs) {
    setup();
    int fd = sftp_fs_open("/old", FA_READ);
    CHECK(fd >= 0);
    unsigned char buf[16];
    const unsigned int at0[2] = {0, 0}, at1[2] = {1, 0};
    for (int i = 0; i < 3; i++) {
        now += SECOND;
        CHECK_EQ(sftp_fs_pread(fd, buf, sizeof(buf), at0), 5);
    }
    CHECK_MEM(buf, "hello", 5);
    CHECK_EQ(sftp_fs_pread(fd, buf, 2, at1), 2);
    CHECK_MEM(buf, "el", 2);
    CHECK_EQ(put(fd, "x", 0), -1);          // FatFs refuses the write: no sync either
    CHECK_EQ(nsyncs, 0);
    CHECK_EQ(sftp_fs_close(fd), 0);
    CHECK_EQ(nsyncs, 0);
}

TEST(a_write_at_an_offset_lands_there) {
    setup();
    int fd = sftp_fs_open("/old", FA_READ | FA_WRITE);
    CHECK_EQ(put(fd, "J", 0), 1);
    CHECK_EQ(put(fd, "!", 5), 1);
    CHECK_EQ(card[card_find("/old")].len, 6);
    CHECK_MEM(card[card_find("/old")].data, "Jello!", 6);
    CHECK_EQ(sftp_fs_close(fd), 0);
}

TEST(bad_handles_are_refused) {
    setup();
    const unsigned int at[2] = {0, 0};
    unsigned char buf[4];
    CHECK_EQ(sftp_fs_pwrite(-1, buf, 1, at), -1);
    CHECK_EQ(sftp_fs_pwrite(8, buf, 1, at), -1);
    CHECK_EQ(sftp_fs_pread(3, buf, 1, at), -1);
    CHECK_EQ(sftp_fs_close(3), -1);
    CHECK_EQ(nsyncs, 0);
}

TEST_MAIN(
    RUN(an_upload_is_synced_once_a_second_has_passed_and_not_before);
    RUN(writes_within_a_second_cause_no_sync);
    RUN(each_handle_keeps_its_own_clock);
    RUN(a_handle_reused_after_close_starts_its_clock_at_the_open);
    RUN(a_failed_sync_is_reported_as_a_failed_write);
    RUN(a_failed_write_is_reported_and_not_synced);
    RUN(a_read_only_handle_never_syncs);
    RUN(a_write_at_an_offset_lands_there);
    RUN(bad_handles_are_refused);
)
