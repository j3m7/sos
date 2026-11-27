// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The log files' platform side (core/logfile.h): the SD card through FatFs, the clock, and the
// boot marker's text. Everything runs on core 0, in the main loop, like every other FatFs user
// (the shell, SFTP, Lua's requests), so the card is never used from two places at once.

#include "core/logfile.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "ff.h"
#include "pico/stdlib.h"

#include "core/fault.h"
#include "core/glitchdet.h"
#include "core/version.h"
#include "core/wdog.h"
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

static void boot_text(char *buf, size_t len) {
    const char *why = fault_last() ? fault_last()
                      : glitchdet_caused_reset() ? "glitch detector"
                      : wdog_caused_reset() ? "watchdog"
                      : "power-on or reboot";
    snprintf(buf, len, "SOS %s, last reset: %s", sos_version, why);
}

static const logfile_hooks_t hooks = {
    now_us, wall_us, sdcard_mounted, sdcard_generation, append, boot_text,
};

// Start writing, and mount the card for it once now: the configuration is loaded, so an
// encrypted card opens. Without a card that's one "mount failed" line; it isn't retried, so the
// log isn't filled with them. Any later mount (a file command, `sd`) starts the files.
void logfile_start(void) {
    logfile_init(&hooks);
    sdcard_mount(NULL);
}
