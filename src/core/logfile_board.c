// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The log files' platform side (core/logfile.h): the SD card through FatFs, the clock, and the
// boot marker's text. Everything runs on core 0, in the main loop, like every other FatFs user
// (the shell, SFTP, Lua's requests), so the card is never used from two places at once.

#include "core/logfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "ff.h"
#include "pico/stdlib.h"

#include "core/config.h"
#include "core/resetwhy.h"
#include "core/settings.h"
#include "core/version.h"
#include "files/files.h"
#include "files/sdcard.h"
#include "time/timesync.h"

static uint64_t now_us(void) { return time_us_64(); }

static bool wall_us(int64_t *us) {
    if (!timesync_clock_set())
        return false;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    *us = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
    return true;
}

// Make /var and /var/log; one that exists already is fine.
static FRESULT make_dirs(const char *path) {
    char dir[32];
    for (const char *slash = strchr(path + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        size_t n = (size_t)(slash - path);
        if (n >= sizeof(dir))
            return FR_INVALID_NAME;
        memcpy(dir, path, n);
        dir[n] = '\0';
        FRESULT fr = f_mkdir(dir);
        if (fr != FR_OK && fr != FR_EXIST)
            return fr;
    }
    return FR_OK;
}

static bool append(const char *path, const char *data, size_t n, const char **why) {
    FIL f;
    FRESULT fr = f_open(&f, path, FA_OPEN_APPEND | FA_WRITE);
    if (fr == FR_NO_PATH && (fr = make_dirs(path)) == FR_OK)
        fr = f_open(&f, path, FA_OPEN_APPEND | FA_WRITE);
    if (fr != FR_OK) {
        *why = files_error(fr);
        return false;
    }
    UINT done = 0;
    fr = f_write(&f, data, (UINT)n, &done);
    FRESULT cr = f_close(&f);               // syncs: the lines are on the card when it returns
    if (fr == FR_OK && done != n)
        fr = FR_DENIED;                     // the card is full
    if (fr == FR_OK)
        fr = cr;
    if (fr != FR_OK) {
        *why = files_error(fr);
        return false;
    }
    return true;
}

static bool rotate_file(const char *path, const char **why) {
    char old[48];
    snprintf(old, sizeof(old), "%s.1", path);
    FILINFO fi;
    FRESULT fr = f_stat(path, &fi);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH)
        return true;                        // nothing since the last rotation: keep its .1
    if (fr == FR_OK)
        fr = f_unlink(old);
    if (fr == FR_OK || fr == FR_NO_FILE)
        fr = f_rename(path, old);
    if (fr != FR_OK && fr != FR_NO_FILE) {     // no file to rotate is fine
        *why = files_error(fr);
        return false;
    }
    return true;
}

#define ROTATED_PATH LOGFILE_DIR "/.rotated"

// The time of the last rotation: Unix seconds in decimal, one line.
static bool load_rotated(int64_t *unix_s) {
    FIL f;
    char buf[24];
    UINT n = 0;
    if (f_open(&f, ROTATED_PATH, FA_READ) != FR_OK)
        return false;
    FRESULT fr = f_read(&f, buf, sizeof(buf) - 1, &n);
    f_close(&f);
    if (fr != FR_OK)
        return false;
    buf[n] = '\0';
    char *end;
    long long v = strtoll(buf, &end, 10);
    if (end == buf || (*end && *end != '\n') || v <= 0)
        return false;                       // damaged: the count starts again
    *unix_s = v;
    return true;
}

static bool save_rotated(int64_t unix_s, const char **why) {
    FIL f;
    char buf[24];
    int len = snprintf(buf, sizeof(buf), "%lld\n", (long long)unix_s);
    FRESULT fr = f_open(&f, ROTATED_PATH, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr == FR_NO_PATH && (fr = make_dirs(ROTATED_PATH)) == FR_OK)
        fr = f_open(&f, ROTATED_PATH, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr == FR_OK) {
        UINT done = 0;
        fr = f_write(&f, buf, (UINT)len, &done);
        FRESULT cr = f_close(&f);
        if (fr == FR_OK && done != (UINT)len)
            fr = FR_DENIED;
        if (fr == FR_OK)
            fr = cr;
    }
    if (fr != FR_OK) {
        *why = files_error(fr);
        return false;
    }
    return true;
}

static void boot_text(char *buf, size_t len) {
    snprintf(buf, len, "SOS %s, last reset: %s", sos_version, resetwhy_text());
}

static void retry_mount(void) { sdcard_mount_retry(); }

static const logfile_hooks_t hooks = {
    now_us, wall_us, sdcard_mounted, sdcard_generation, append, boot_text, retry_mount,
    rotate_file, load_rotated, save_rotated, sdcard_last_mount_us,
};

void logfile_apply_settings(void) {
    const sos_config_t *cfg = settings();
    logfile_set_rotation(cfg->log_rotate_days ? cfg->log_rotate_days
                                              : CONFIG_LOG_ROTATE_DAYS_DEFAULT,
                         cfg->log_rotate_minute);
}

// Start writing, and mount the card for it once now: the configuration is loaded, so an
// encrypted card opens. Without a card that's one "mount failed" line; the writer then retries
// quietly every minute (sdcard_mount_retry), and any other mount starts the files too.
void logfile_start(void) {
    logfile_init(&hooks);
    logfile_apply_settings();
    sdcard_mount(NULL);
}
