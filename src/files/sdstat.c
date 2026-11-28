// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See sdstat.h, and its rule: everything here counts and returns. It calls countcheck_sd_kind()
// (pure, string compares) and nothing else; the `sdstat-leaf` test checks that.

#include "files/sdstat.h"

#include "core/countcheck.h"

sdcard_counters_t sdstat;
bool sdstat_initialising;

static void count(countcheck_sd_kind_t kind) {
    switch (kind) {
    case COUNTCHECK_SD_CRC:     SDSTAT_COUNT(crc, 1);     break;
    case COUNTCHECK_SD_CMD:     SDSTAT_COUNT(cmd, 1);     break;
    case COUNTCHECK_SD_WRITE:   SDSTAT_COUNT(write, 1);   break;
    case COUNTCHECK_SD_TIMEOUT: SDSTAT_COUNT(timeout, 1); break;
    case COUNTCHECK_SD_NONE:    break;
    }
}

void sdstat_error_line(const char *func) {
    count(countcheck_sd_kind(func, sdstat_initialising, true));
}

// The SD library's debug lines. Only its sd_card_spi.c is built with them (USE_DBG_PRINTF, set on
// that file alone in CMakeLists.txt), and this strong definition replaces the library's weak one,
// which formats into a 256-byte buffer. None is formatted or printed: each is counted by the
// function it comes from, never by its text (core/countcheck_names.h), and dropped; its
// arguments aren't read. Declared here, not through the library's my_debug.h, so the host tests
// build this file without the library.
int debug_message_printf(const char *func, int line, const char *fmt, ...);
int debug_message_printf(const char *func, int line, const char *fmt, ...) {
    (void)line;
    (void)fmt;
    count(countcheck_sd_kind(func, sdstat_initialising, false));
    return 0;
}
