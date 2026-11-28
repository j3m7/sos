// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/countcheck.c, the pure half of phase 6's checks (docs/design/health.md
// §9.9): the windows, `wifi`, `sd-bus` and `net-pools` from made-up inputs, one test per status
// path at least, and the classifiers the counting hooks use, by the libraries' function names.

#include <stdio.h>
#include <string.h>

#include "core/countcheck.h"
#include "test.h"

static health_result_t r;

// A text of HEALTH_TEXT_MAX - 1 characters may have been cut: every text tested must be shorter.
static bool is(const char *text) {
    CHECK(strlen(r.text) < HEALTH_TEXT_MAX - 1);
    if (strcmp(r.text, text))
        fprintf(stderr, "  text: \"%s\", expected \"%s\"\n", r.text, text);
    return !strcmp(r.text, text);
}

// --- windows -------------------------------------------------------------------------------------

static uint32_t span;
static uint32_t d[COUNTCHECK_VALUES];

static void at(countcheck_window_t *w, uint32_t now, uint32_t a) {
    uint32_t v[COUNTCHECK_VALUES] = {a, 2 * a, 0};
    countcheck_window(w, now, v, d, &span);
}

TEST(window_from_boot) {
    countcheck_window_t w = {0};
    at(&w, 120, 5);                     // first run: from boot
    CHECK_EQ(d[0], 5);
    CHECK_EQ(d[1], 10);
    CHECK_EQ(span, 120);
    CHECK_EQ(w.n, 1);
    at(&w, 300, 7);                     // under the gap: no new sample, still from boot
    CHECK_EQ(d[0], 7);
    CHECK_EQ(span, 300);
    CHECK_EQ(w.n, 1);
}

TEST(window_an_hour) {
    countcheck_window_t w = {0};
    for (uint32_t t = 120, c = 0; t <= 120 + 600 * 7; t += 600, c += 2)
        at(&w, t, c);                   // 8 samples, 2 more each 10 min
    CHECK_EQ(w.n, 8);
    // At 120 + 4200 + 600 = 4920: the newest sample at least an hour old is at 1320 (count 4).
    at(&w, 4920, 16);
    CHECK_EQ(span, 3600);
    CHECK_EQ(d[0], 12);
    // An extra run 30 s later changes nothing but the end: same base, no new sample.
    unsigned n = w.next;
    at(&w, 4950, 16);
    CHECK_EQ(w.next, n);
    CHECK_EQ(span, 3630);
    CHECK_EQ(d[0], 12);
}

TEST(window_old_errors_age_out) {
    countcheck_window_t w = {0};
    at(&w, 100, 0);
    at(&w, 700, 9);                     // nine errors early on
    for (uint32_t t = 1300; t <= 9000; t += 600)
        at(&w, t, 9);                   // none since
    CHECK_EQ(d[0], 0);
    CHECK(span >= 3600 && span < 3600 + 600);
}

TEST(window_full_ring_always_has_a_base) {
    countcheck_window_t w = {0};
    uint32_t t = 50;
    for (unsigned i = 0; i < 40; i++, t += 600 + i * 7)
        at(&w, t, i);
    CHECK(span >= 3600);
    CHECK(span < 3600 + 600 + 40 * 7);
}

// --- wifi ----------------------------------------------------------------------------------------

static countcheck_wifi_t up(void) {
    countcheck_wifi_t in = {
        .state = COUNTCHECK_WIFI_UP, .span_s = 3600, .fw = "7.95.49", .driver = "1.1.0",
    };
    return in;
}

TEST(wifi_pass) {
    countcheck_wifi_t in = up();
    countcheck_judge_wifi(&in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(is("fw 7.95.49, driver 1.1.0; no errors in 1 h; no drops in 24 h"));
    in.timeouts = 1;
    in.bus = 1;
    in.drops = 1;
    countcheck_judge_wifi(&in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(is("fw 7.95.49, driver 1.1.0; 2 errors in 1 h; 1 drop in 24 h"));
    in = up();
    in.span_s = 125;                    // before an hour of uptime
    countcheck_judge_wifi(&in, &r);
    CHECK(is("fw 7.95.49, driver 1.1.0; no errors in 2 min; no drops in 24 h"));
}

TEST(wifi_pass_short_down) {
    countcheck_wifi_t in = up();
    in.state = COUNTCHECK_WIFI_DOWN;
    in.down_s = 300;
    countcheck_judge_wifi(&in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(is("no connection for 5 min; fw 7.95.49; no errors in 1 h"));
}

TEST(wifi_errors) {
    countcheck_wifi_t in = up();
    in.timeouts = 5;
    in.bus = 2;
    countcheck_judge_wifi(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("7 driver errors in 1 h (5 timeouts, 2 bus); fw 7.95.49"));
    in = up();
    in.io = 3;
    countcheck_judge_wifi(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("3 driver errors in 1 h (3 I/O); fw 7.95.49"));
    in.timeouts = 1;
    in.io = 1;
    in.bus = 0;
    countcheck_judge_wifi(&in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);    // two: under the line
    in = up();
    in.timeouts = 11;
    in.io = 12;
    in.bus = 6;
    countcheck_judge_wifi(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);    // 29: the fw left out to fit
    CHECK(is("29 driver errors in 1 h (11 timeouts, 12 I/O, 6 bus)"));
    in.bus = 7;
    countcheck_judge_wifi(&in, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(is("30 driver errors in 1 h: the chip isn't answering"));
}

TEST(wifi_drops) {
    countcheck_wifi_t in = up();
    in.drops = 3;
    countcheck_judge_wifi(&in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    in.drops = 4;
    in.last_drop_ago_s = 2 * 3600 + 5;
    countcheck_judge_wifi(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("4 link drops in 24 h, the last 2 h ago; fw 7.95.49"));
    in.last_drop_ago_s = 30;
    countcheck_judge_wifi(&in, &r);
    CHECK(is("4 link drops in 24 h, the last 30 s ago; fw 7.95.49"));
}

TEST(wifi_down_long) {
    countcheck_wifi_t in = up();
    in.state = COUNTCHECK_WIFI_DOWN;
    in.down_s = 1799;
    countcheck_judge_wifi(&in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    in.down_s = 42 * 60;
    countcheck_judge_wifi(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("no connection for 42 min: check the network and 'wifi'"));
    in.down_s = 3 * 86400;
    countcheck_judge_wifi(&in, &r);
    CHECK(is("no connection for 3 d: check the network and 'wifi'"));
}

TEST(wifi_no_version_and_skip) {
    countcheck_wifi_t in = up();
    in.fw = NULL;
    countcheck_judge_wifi(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("the chip didn't give its firmware version"));
    in.fw = "";
    in.timeouts = 3;
    countcheck_judge_wifi(&in, &r);
    CHECK(is("3 driver errors in 1 h (3 timeouts); fw ?"));
    in = up();
    in.state = COUNTCHECK_WIFI_UNSET;
    countcheck_judge_wifi(&in, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
    CHECK(is("Wi-Fi not set up"));
    in.state = COUNTCHECK_WIFI_STOPPED;
    countcheck_judge_wifi(&in, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
    CHECK(is("Wi-Fi stopped"));
}

TEST(wifi_drop_times) {
    uint32_t t[8] = {100, 5000, 90000, 90100, 0, 0, 0, 0};
    uint32_t ago;
    CHECK_EQ(countcheck_drops_within(t, 4, 90200, 86400, &ago), 3);    // 100 is too old
    CHECK_EQ(ago, 100);
    CHECK_EQ(countcheck_drops_within(t, 4, 200000, 86400, &ago), 0);
    CHECK_EQ(ago, 0);
    CHECK_EQ(countcheck_drops_within(t, 0, 200000, 86400, &ago), 0);
    CHECK_EQ(countcheck_drops_within(t, 2, 4000, 86400, &ago), 1);     // one still to come
}

TEST(wifi_fw_version) {
    char v[16];
    CHECK(countcheck_fw_version("wl0: Nov 29 2021 22:50:20 version 7.95.49 (2271bb6 CY) "
                                "FWID 01-c51d9400\n", v, sizeof(v)));
    CHECK(!strcmp(v, "7.95.49"));
    CHECK(!countcheck_fw_version("wl0: no version here", v, sizeof(v)));
    CHECK(!strcmp(v, ""));
    CHECK(!countcheck_fw_version(NULL, v, sizeof(v)));
    CHECK(countcheck_fw_version("version 7.95.49", v, 4));
    CHECK(!strcmp(v, "7.9"));
}

TEST(wifi_warn_kinds) {
    CHECK_EQ(countcheck_wifi_warn_kind("cyw43_ll_sdpcm_poll_device"), COUNTCHECK_WIFI_BUS);
    CHECK_EQ(countcheck_wifi_warn_kind("cyw43_kso_set"), COUNTCHECK_WIFI_BUS);
    CHECK_EQ(countcheck_wifi_warn_kind("cyw43_ll_bus_sleep_helper"), COUNTCHECK_WIFI_BUS);
    CHECK_EQ(countcheck_wifi_warn_kind("cyw43_netif_output"), COUNTCHECK_WIFI_TX);
    CHECK_EQ(countcheck_wifi_warn_kind("cyw43_do_ioctl"), COUNTCHECK_WIFI_NONE);
    CHECK_EQ(countcheck_wifi_warn_kind("cyw43_sdpcm_send_common"), COUNTCHECK_WIFI_NONE);
    CHECK_EQ(countcheck_wifi_warn_kind("cyw43_ll_bus_init"), COUNTCHECK_WIFI_OTHER);
    CHECK_EQ(countcheck_wifi_warn_kind(NULL), COUNTCHECK_WIFI_OTHER);
}

// --- sd-bus --------------------------------------------------------------------------------------

#define MB (1024ull * 1024)

static countcheck_sd_t card(void) {
    countcheck_sd_t in = {.ever_ok = true, .khz = 25000, .bytes = 37 * MB};
    return in;
}

TEST(sd_pass) {
    countcheck_sd_t in = card();
    countcheck_judge_sd(&in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(is("25.0 MHz; 37 MB since the card's init, no errors"));
    in.bytes = 512 * 1024;
    countcheck_judge_sd(&in, &r);
    CHECK(is("25.0 MHz; 512 KB since the card's init, no errors"));
    in.bytes = 12ull * 1024 * MB;
    countcheck_judge_sd(&in, &r);
    CHECK(is("25.0 MHz; 12 GB since the card's init, no errors"));
}

TEST(sd_pass_fast_clock) {
    countcheck_sd_t in = card();
    in.khz = 37500;
    in.bytes = 3 * MB;
    countcheck_judge_sd(&in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);    // the clock alone is no finding
    CHECK(is("at 37.5 MHz; 3 MB since the card's init, no errors"));
}

TEST(sd_retried) {
    countcheck_sd_t in = card();
    in.bytes = 412 * MB;
    in.crc = 2;
    countcheck_judge_sd(&in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(is("25.0 MHz; 2 retried in 412 MB (under 1 per 10 MB)"));
    in.crc = 3;                         // three, but 1 per 137 MB: under the rate
    countcheck_judge_sd(&in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    in.bytes = 31 * MB;
    in.crc = 10;
    in.cmd = 3;
    in.write = 1;
    countcheck_judge_sd(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("25.0 MHz; 14 retried in 31 MB: over 1 per 10 MB"));
    in = card();
    in.bytes = 100 * 1024;
    in.crc = 2;                         // two after 100 KB: under the floor of three
    countcheck_judge_sd(&in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    in.crc = 3;
    in.bytes = 30 * MB;                 // exactly 1 per 10 MB
    countcheck_judge_sd(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
}

TEST(sd_retried_fast) {
    countcheck_sd_t in = card();
    in.khz = 37500;
    in.bytes = 400 * MB;
    in.crc = 2;
    countcheck_judge_sd(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("2 retried at 37.5 MHz, over SD's 25 MHz: sd speed 25"));
}

TEST(sd_failed) {
    countcheck_sd_t in = card();
    in.failed_writes = 2;
    in.failed_reads = 1;
    countcheck_judge_sd(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("2 failed writes, 1 failed read since init: sd check"));
    in.failed_writes = 0;
    countcheck_judge_sd(&in, &r);
    CHECK(is("1 failed read since init: sd check"));
    in.failed_writes = 1;
    in.failed_reads = 0;
    countcheck_judge_sd(&in, &r);
    CHECK(is("1 failed write since init: sd check"));
    in.failed_writes = 4;
    in.failed_reads = 5;
    countcheck_judge_sd(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    in.failed_reads = 8;
    countcheck_judge_sd(&in, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(is("12 failed reads/writes since init: card or bus failing"));
}

TEST(sd_init_and_timeouts) {
    countcheck_sd_t in = card();
    in.khz = 75000;
    in.bytes = 0;
    in.last_init_failed = true;
    in.cmd = 4;
    in.timeouts = 1;
    countcheck_judge_sd(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("card no longer initialises at 75.0 MHz (removed?)"));
    in = card();
    in.timeouts = 2;
    countcheck_judge_sd(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("card timed out 2x since init (2 s each); 25.0 MHz"));
    in.failed_reads = 1;                // a failed read leads
    countcheck_judge_sd(&in, &r);
    CHECK(is("1 failed read since init: sd check"));
    in = card();
    in.ever_ok = false;
    in.last_init_failed = true;
    countcheck_judge_sd(&in, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
    CHECK(is("no card initialised since boot"));
}

TEST(sd_kinds) {
    CHECK_EQ(countcheck_sd_kind("chk_crc16", false, false), COUNTCHECK_SD_CRC);
    CHECK_EQ(countcheck_sd_kind("chk_crc16", true, false), COUNTCHECK_SD_CRC);
    CHECK_EQ(countcheck_sd_kind("sd_wait_token", false, false), COUNTCHECK_SD_TIMEOUT);
    CHECK_EQ(countcheck_sd_kind("sd_wait_ready", true, false), COUNTCHECK_SD_TIMEOUT);
    CHECK_EQ(countcheck_sd_kind("in_sd_spi_sectors", true, false), COUNTCHECK_SD_CMD);
    CHECK_EQ(countcheck_sd_kind("sd_cmd", false, false), COUNTCHECK_SD_CMD);
    CHECK_EQ(countcheck_sd_kind("sd_cmd", true, false), COUNTCHECK_SD_NONE);   // the card's type
    // send_block: its error line (the card refused the block), not its debug lines (which
    // repeat sd_wait_ready's timeout).
    CHECK_EQ(countcheck_sd_kind("send_block", false, true), COUNTCHECK_SD_WRITE);
    CHECK_EQ(countcheck_sd_kind("send_block", false, false), COUNTCHECK_SD_NONE);
    CHECK_EQ(countcheck_sd_kind("chk_crc16", false, true), COUNTCHECK_SD_NONE);
    // The callers' repeats of an event counted above, and the initialisation's own lines.
    CHECK_EQ(countcheck_sd_kind("in_sd_read_blocks", false, false), COUNTCHECK_SD_NONE);
    CHECK_EQ(countcheck_sd_kind("read_bytes", false, false), COUNTCHECK_SD_NONE);
    CHECK_EQ(countcheck_sd_kind("stop_wr_tran", false, false), COUNTCHECK_SD_NONE);
    CHECK_EQ(countcheck_sd_kind("sd_write_blocks", false, false), COUNTCHECK_SD_NONE);
    CHECK_EQ(countcheck_sd_kind("sd_card_spi_init", true, true), COUNTCHECK_SD_NONE);
    CHECK_EQ(countcheck_sd_kind("sd_init_medium", true, true), COUNTCHECK_SD_NONE);
    CHECK_EQ(countcheck_sd_kind(NULL, false, false), COUNTCHECK_SD_NONE);
}

// --- net-pools -----------------------------------------------------------------------------------

TEST(pools) {
    countcheck_pools_t in = {.span_s = 3600, .pool_max = 5, .pool_avail = 12};
    countcheck_judge_pools(&in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(is("no failures in 1 h; packets most 5 of 12"));
    in.heap = 2;
    countcheck_judge_pools(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("lwIP heap full 2x in 1 h: sends refused"));
    in.seg = 4;
    countcheck_judge_pools(&in, &r);
    CHECK(is("segment pool full 4x in 1 h: sends refused"));
    in.pbuf = 3;
    in.span_s = 1500;
    countcheck_judge_pools(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("packet pool empty 3x in 25 min: received frames dropped"));
}

TEST_MAIN(
    RUN(window_from_boot);
    RUN(window_an_hour);
    RUN(window_old_errors_age_out);
    RUN(window_full_ring_always_has_a_base);
    RUN(wifi_pass);
    RUN(wifi_pass_short_down);
    RUN(wifi_errors);
    RUN(wifi_drops);
    RUN(wifi_down_long);
    RUN(wifi_no_version_and_skip);
    RUN(wifi_drop_times);
    RUN(wifi_fw_version);
    RUN(wifi_warn_kinds);
    RUN(sd_pass);
    RUN(sd_pass_fast_clock);
    RUN(sd_retried);
    RUN(sd_retried_fast);
    RUN(sd_failed);
    RUN(sd_init_and_timeouts);
    RUN(sd_kinds);
    RUN(pools);
)
