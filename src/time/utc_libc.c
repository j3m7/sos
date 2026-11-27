// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// localtime_r() and mktime() for a board that keeps UTC and has no time zone.
//
// The C library's versions consult the time zone, and its time zone code parses the TZ
// variable with sscanf(). Two libraries call them: the SD card library stamps files with
// localtime_r() (get_fattime), and wolfSSH's SFTP server converts file times with both. That
// chain was the only thing in the image that used scanf, about 10 KB with it (issue #123).
//
// The linker is told to send both names here (-Wl,--wrap in CMakeLists.txt), so neither
// library needs changing. With no TZ set the C library's answers are UTC too, so nothing that
// calls them sees a difference.

#include <time.h>

#include "time/timeconv.h"

// Reached only through -Wl,--wrap, so no header declares them.
struct tm *__wrap_localtime_r(const time_t *t, struct tm *result);
time_t __wrap_mktime(struct tm *tm);

struct tm *__wrap_localtime_r(const time_t *t, struct tm *result) {
    return gmtime_r(t, result);
}

// Like mktime(): the fields may be out of range and are normalized in place, with the weekday
// and day of the year filled in.
time_t __wrap_mktime(struct tm *tm) {
    time_t t = (time_t)timeconv_from_tm(tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_hour,
                                        tm->tm_min, tm->tm_sec);
    gmtime_r(&t, tm);
    return t;
}
