// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/bgcheck.c, the pure half of phase 4's checks (docs/design/health.md
// §9.5): `resets`, `rtc-drift`, `heap-sum`, `core1` and the brownout detector's setting, each
// from made-up inputs, one test per status path at least; and the drift arithmetic in
// src/time/timeconv.c that `rtc-drift` and `time` share.

#include <stdio.h>
#include <string.h>

#include "core/bgcheck.h"
#include "test.h"

static health_result_t r;

// A text of HEALTH_TEXT_MAX - 1 characters may have been cut: every text tested must be shorter.
static bool says(const char *part) {
    CHECK(strlen(r.text) < HEALTH_TEXT_MAX - 1);
    if (!strstr(r.text, part))
        fprintf(stderr, "  text: \"%s\" lacks \"%s\"\n", r.text, part);
    return strstr(r.text, part) != NULL;
}

static bool is(const char *text) {
    CHECK(strlen(r.text) < HEALTH_TEXT_MAX - 1);
    if (strcmp(r.text, text))
        fprintf(stderr, "  text: \"%s\", expected \"%s\"\n", r.text, text);
    return !strcmp(r.text, text);
}

// --- resets ------------------------------------------------------------------------------------------

static bgcheck_resets_t res(resetwhy_t why, const char *text) {
    bgcheck_resets_t in = {why, text, HISTORY_OURS, 41, 0, 0};
    return in;
}

TEST(resets_pass) {
    bgcheck_resets_t in = res(RESETWHY_POWER_ON, "power-on");
    bgcheck_judge_resets(&in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(is("power-on; 41 boots recorded, no loop"));
    in = res(RESETWHY_REBOOT, "reboot");
    in.boots = 1;
    bgcheck_judge_resets(&in, &r);
    CHECK(is("reboot; 1 boot recorded, no loop"));
    in = res(RESETWHY_RESET_PIN, "reset pin");
    in.boots = 0;
    bgcheck_judge_resets(&in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(is("reset pin; no boots recorded"));
}

TEST(resets_warn_reasons) {
    bgcheck_resets_t in = res(RESETWHY_BROWNOUT, "brownout");
    bgcheck_judge_resets(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("last reset: brownout (core supply under 0.95 V)"));
    in = res(RESETWHY_WATCHDOG, "watchdog timeout");
    bgcheck_judge_resets(&in, &r);
    CHECK(is("last reset: watchdog timeout"));
    in = res(RESETWHY_GLITCH, "glitch detector");
    bgcheck_judge_resets(&in, &r);
    CHECK(is("last reset: glitch detector"));
    in = res(RESETWHY_DEBUGGER, "debugger");
    bgcheck_judge_resets(&in, &r);
    CHECK(is("last reset: debugger"));
    in = res(RESETWHY_UNKNOWN, "unknown (CHIP_RESET 0x00000000)");
    bgcheck_judge_resets(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("last reset: unknown (CHIP_RESET 0x00000000)"));
}

TEST(resets_fault_record) {
    bgcheck_resets_t in = res(RESETWHY_FAULT, "hard fault at 0x10001234");
    bgcheck_judge_resets(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("last reset: hard fault at 0x10001234"));
    in.text = "hard fault on core 0 at 0x1000409c, called from 0x1000409b (CFSR 0x00000100)";
    bgcheck_judge_resets(&in, &r);
    CHECK(is("last reset: hard fault on core 0 at 0x1000409c (see uptime)"));
}

TEST(resets_loop_first) {
    bgcheck_resets_t in = res(RESETWHY_FAULT, "panic: x");
    in.short_runs = 3;
    bgcheck_judge_resets(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("3 short runs in a row; last reset: fault"));
    in.short_runs = 2;
    bgcheck_judge_resets(&in, &r);
    CHECK(says("last reset: panic"));
    in = res(RESETWHY_WATCHDOG, "watchdog timeout");
    in.short_runs = 12;
    bgcheck_judge_resets(&in, &r);
    CHECK(is("12 short runs in a row; last reset: watchdog timeout"));
}

TEST(resets_history_trouble) {
    bgcheck_resets_t in = res(RESETWHY_REBOOT, "reboot");
    in.damaged = 2;
    bgcheck_judge_resets(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("history: 2 damaged records skipped; last reset: reboot"));
    in.damaged = 0;
    in.history = HISTORY_ABSENT;
    bgcheck_judge_resets(&in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("reboot; history off: the image overlaps its sector"));
    in.history = HISTORY_NEWER;
    bgcheck_judge_resets(&in, &r);
    CHECK(is("reboot; history sector of a newer format: not written"));
    // A reason worth a WARN says more than the sector's state.
    in = res(RESETWHY_BROWNOUT, "brownout");
    in.history = HISTORY_ABSENT;
    bgcheck_judge_resets(&in, &r);
    CHECK(says("brownout (core"));
}

// --- the drift arithmetic (timeconv.c) ------------------------------------------------------------

TEST(drift_arithmetic) {
    CHECK_EQ(timeconv_rtc_unc_ms(0, 50), 1050);
    CHECK_EQ(timeconv_rtc_unc_ms(21, 0), 1000 + 21 * 32);
    CHECK_EQ(timeconv_rtc_unc_ms(-21, 0), 1000 + 21 * 32);
    CHECK_EQ(timeconv_rate_mppm(5419, 59051), 91768);       // docs/time.md's example: 92 ppm
    CHECK_EQ(timeconv_rate_mppm(-1000, 100000), -10000);
    CHECK_EQ(timeconv_rate_mppm(1, 0), 0);
    CHECK_EQ(timeconv_verdict(92000, 13000, 20000), 1);
    CHECK_EQ(timeconv_verdict(-92000, 13000, 20000), 1);
    CHECK_EQ(timeconv_verdict(1000, 19000, 20000), -1);
    CHECK_EQ(timeconv_verdict(1000, 19001, 20000), 0);      // straddles: never judged
    CHECK_EQ(timeconv_verdict(33000, 13000, 20000), 0);     // 20 is inside 33 +- 13
    CHECK_EQ(timeconv_cal_mppm(21), 91140);
    char b[48];
    timeconv_format_rate(b, sizeof(b), 92100, 12700);
    CHECK(!strcmp(b, "+92.1+-13 ppm"));
    timeconv_format_rate(b, sizeof(b), -1800, 600);
    CHECK(!strcmp(b, "-1.8+-0.6 ppm"));
    timeconv_format_rate(b, sizeof(b), 1210400, 3000);
    CHECK(!strcmp(b, "+1210+-3.0 ppm"));
    timeconv_format_cal(b, sizeof(b), 0);
    CHECK(!strcmp(b, "no cal"));
    timeconv_format_cal(b, sizeof(b), 21);
    CHECK(!strcmp(b, "cal 91.1"));
    timeconv_format_cal(b, sizeof(b), -3);
    CHECK(!strcmp(b, "cal -13.0"));
    timeconv_format_ppm(b, sizeof(b), 92040);
    CHECK(!strcmp(b, "92"));
    timeconv_format_ppm(b, sizeof(b), 114540);
    CHECK(!strcmp(b, "114.5"));
    timeconv_format_ppm(b, sizeof(b), -13360);
    CHECK(!strcmp(b, "-13.4"));
    timeconv_format_span(b, sizeof(b), 2700);
    CHECK(!strcmp(b, "45 min"));
    timeconv_format_span(b, sizeof(b), 22 * 3600 + 100);
    CHECK(!strcmp(b, "22 h"));
    timeconv_format_span(b, sizeof(b), 5 * 86400);
    CHECK(!strcmp(b, "5 d"));
}

// --- rtc-drift -----------------------------------------------------------------------------------

static timeconv_drift_t measured(int64_t err_ms, uint32_t interval_s, int steps) {
    timeconv_drift_t d = {0};
    d.rtc_present = true;
    d.rtc = TIMECONV_DRIFT_MEASURED;
    d.rtc_err_ms = err_ms;
    d.rtc_unc_ms = timeconv_rtc_unc_ms(steps, 0);
    d.rtc_interval_s = interval_s;
    d.cal_steps = steps;
    return d;
}

TEST(drift_rtc_warn_uncalibrated) {
    // dev-1 as it was: 92 ppm fast, no calibration, 22 h between syncs.
    timeconv_drift_t d = measured(7286, 22 * 3600, 0);
    bgcheck_judge_drift(&d, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("RTC +92.0+-13 ppm in 22 h, no cal: try time cal 92"));
}

TEST(drift_rtc_warn_calibrated) {
    // 35 ppm left over after 48 h (+-9.7 at 21 steps): surely over 20.
    timeconv_drift_t d = measured(6048, 48 * 3600, 21);
    bgcheck_judge_drift(&d, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("RTC +35.0+-9.7 ppm in 48 h, cal 91.1: try time cal 126.1"));
    // ...slow by as much: a smaller calibration.
    d = measured(-6048, 48 * 3600, 21);
    bgcheck_judge_drift(&d, &r);
    CHECK(says("try time cal 56.1"));
    // 23.4 ppm over 24 h at 21 steps (+-19.4) isn't sure.
    d = measured(2022, 24 * 3600, 21);
    bgcheck_judge_drift(&d, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
}

TEST(drift_rtc_beyond_range) {
    timeconv_drift_t d = measured(24552, 22 * 3600, 0);     // 310 ppm
    bgcheck_judge_drift(&d, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("RTC +310+-13 ppm in 22 h: beyond time cal's range"));
}

TEST(drift_rtc_pass) {
    timeconv_drift_t d = measured(156, 48 * 3600, 21);      // 0.9 ppm, +-9.7
    bgcheck_judge_drift(&d, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(is("RTC +0.9+-9.7 ppm in 48 h, cal 91.1"));
    // A calibrated RTC needs over a day: 0.9 + 19.4 is over 20 at 24 h.
    d = measured(78, 24 * 3600, 21);
    bgcheck_judge_drift(&d, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
}

TEST(drift_rtc_too_short) {
    // 6 hours: +-48 ppm can't say whether 5 ppm is within 20.
    timeconv_drift_t d = measured(108, 6 * 3600, 0);
    bgcheck_judge_drift(&d, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
    CHECK(is("RTC +5.0+-46 ppm in 6 h, no cal: too short to judge"));
    // ...but 92 ppm is surely over 20 after 4 hours.
    d = measured(1325, 4 * 3600, 0);
    bgcheck_judge_drift(&d, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    // The minimum gap for a sure PASS uncalibrated: 1.05 s / 20 ppm = 52 500 s, 14.6 h.
    d = measured(0, 52500, 0);
    d.rtc_unc_ms = timeconv_rtc_unc_ms(0, 50);
    bgcheck_judge_drift(&d, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    d.rtc_interval_s = 52000;
    bgcheck_judge_drift(&d, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
}

TEST(drift_rtc_not_measured) {
    timeconv_drift_t d = {0};
    d.rtc_present = true;
    d.rtc = TIMECONV_DRIFT_NO_BASELINE;
    bgcheck_judge_drift(&d, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
    CHECK(is("RTC: no baseline; drift after two syncs (no cal)"));
    d.rtc = TIMECONV_DRIFT_BASELINE;
    d.rtc_interval_s = 3 * 3600;
    bgcheck_judge_drift(&d, &r);
    CHECK(is("RTC set 3 h ago (no cal): drift at the next sync"));
    d.rtc_interval_s = 0;
    d.cal_steps = 21;
    bgcheck_judge_drift(&d, &r);
    CHECK(is("RTC set at a sync (cal 91.1): drift at the next"));
    d.rtc = TIMECONV_DRIFT_CAL_CHANGED;
    bgcheck_judge_drift(&d, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
    CHECK(is("calibration changed since the RTC was set; next sync"));
}

TEST(drift_xosc) {
    timeconv_drift_t d = measured(156, 48 * 3600, 21);
    d.xosc = true;
    d.xosc_err_ms = -156;                       // -1.8 ppm over 24 h
    d.xosc_unc_ms = 50;
    d.xosc_interval_s = 24 * 3600;
    bgcheck_judge_drift(&d, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(is("RTC +0.9+-9.7 ppm in 48 h, cal 91.1"));    // the XOSC part doesn't fit
    timeconv_drift_t e = measured(389, 5 * 86400, 0);
    e.xosc = true;
    e.xosc_err_ms = -156;
    e.xosc_unc_ms = 50;
    e.xosc_interval_s = 24 * 3600;
    bgcheck_judge_drift(&e, &r);
    CHECK(is("RTC +0.9+-2.3 ppm in 5 d, no cal; XOSC -1.8+-0.6 ppm in 24 h"));
    // The XOSC worse than the RTC leads the text.
    d.xosc_err_ms = 6150;                       // 71.2 ppm
    d.xosc_unc_ms = 80;
    bgcheck_judge_drift(&d, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(says("XOSC +71.2+-0.9 ppm in 24 h (crystal: 30+30 ppm)"));
    d.xosc_err_ms = 104544;                     // 1210 ppm
    bgcheck_judge_drift(&d, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(says("XOSC +1210+-0.9 ppm: beyond the chip's 1000 ppm margin"));
    // Too short for the XOSC alone: the RTC's PASS stands.
    d.xosc_err_ms = 10;
    d.xosc_unc_ms = 100;
    d.xosc_interval_s = 600;
    bgcheck_judge_drift(&d, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
}

TEST(drift_no_rtc) {
    timeconv_drift_t d = {0};
    bgcheck_judge_drift(&d, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
    CHECK(is("no RTC; XOSC not measured (two syncs in one uptime)"));
    d.xosc = true;
    d.xosc_err_ms = -156;
    d.xosc_unc_ms = 50;
    d.xosc_interval_s = 24 * 3600;
    bgcheck_judge_drift(&d, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(is("no RTC; XOSC -1.8+-0.6 ppm in 24 h"));
}

// --- heap-sum ----------------------------------------------------------------------------------------

static bgcheck_heap_t heap_ok(void) {
    bgcheck_heap_t h = {0};
    h.a.used = 96 * 1024;
    h.a.free = 114 * 1024;
    h.a.arena = 210 * 1024;
    h.a.tags = 81 * 1024;
    h.b = h.a;
    h.start = 0x20010000u;
    h.end = h.start + h.a.arena;
    h.limit = h.start + 380 * 1024;
    h.msplim = h.limit;
    return h;
}

TEST(heap_pass) {
    bgcheck_heap_t h = heap_ok();
    bgcheck_judge_heap(&h, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(is("in use 96 KB (tags 81 KB), arena 210 KB of 380 KB"));
    h.msplim = 0;                               // no guard: nothing to compare with
    bgcheck_judge_heap(&h, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
}

TEST(heap_tags_over) {
    bgcheck_heap_t h = heap_ok();
    h.a.tags = 312 * 1024;
    bgcheck_judge_heap(&h, &r);
    CHECK_EQ(r.status, HEALTH_PASS);            // once: a realloc between two counts
    h.b.tags = 312 * 1024;
    h.b.used = 120 * 1024;
    bgcheck_judge_heap(&h, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("tags hold 312 KB, heap in use 120 KB: a double release?"));
}

TEST(heap_mallinfo_inconsistent) {
    bgcheck_heap_t h = heap_ok();
    h.a.arena = 212 * 1024;
    h.end = h.start + h.a.arena;
    bgcheck_judge_heap(&h, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("mallinfo: in use + free 210 KB, arena 212 KB"));
    h = heap_ok();
    h.end = h.start + 200 * 1024;
    bgcheck_judge_heap(&h, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("heap ends 200 KB in, arena 210 KB"));
}

TEST(heap_end_past_guard) {
    bgcheck_heap_t h = heap_ok();
    h.msplim = h.end - 16;
    bgcheck_judge_heap(&h, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(says("beyond the stack guard"));
    h = heap_ok();
    h.end = h.limit + 8;
    bgcheck_judge_heap(&h, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(says("beyond its limit"));
}

// --- core1 -------------------------------------------------------------------------------------------

#define S(x) ((uint64_t)(x) * 1000000u)

TEST(core1_paths) {
    bgcheck_core1_state_t st = {0};
    bgcheck_core1_in_t in = {false, false, BGCHECK_WAIT_NONE, 0, S(10), 0, NULL};
    bgcheck_judge_core1(&st, &in, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
    CHECK(is("no script running"));

    in = (bgcheck_core1_in_t){true, false, BGCHECK_WAIT_NONE, 100, S(20), 2, "loop.lua"};
    bgcheck_judge_core1(&st, &in, &r);          // first sight
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(is("loop.lua running 2 s"));
    in.beats = 98100;
    in.now_us = S(30);
    in.run_s = 12;
    bgcheck_judge_core1(&st, &in, &r);
    CHECK(is("loop.lua running 12 s, 9800 hooks/s"));

    // Waiting for core 0 (sos.http) isn't a hang, however long.
    in.waiting = BGCHECK_WAIT_CORE0;
    in.now_us = S(100);
    in.run_s = 82;
    bgcheck_judge_core1(&st, &in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(is("loop.lua waiting for core 0, running 82 s"));

    // A long C call: no beat, not waiting.
    in.waiting = BGCHECK_WAIT_NONE;
    in.now_us = S(110);
    bgcheck_judge_core1(&st, &in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    in.now_us = S(131);
    bgcheck_judge_core1(&st, &in, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("loop.lua in one C call for 31 s: Ctrl-C waits for it"));

    // Stopped: 10 s with no beat after the stop is a FAIL.
    in.cancel = true;
    in.now_us = S(135);
    bgcheck_judge_core1(&st, &in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);            // the stop is 0 s old
    in.now_us = S(175);
    bgcheck_judge_core1(&st, &in, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    CHECK(is("core 1 ignored stop for 40 s: no new script until it returns"));

    // It ends at last.
    in.running = false;
    in.now_us = S(185);
    st.run_s = 52;
    bgcheck_judge_core1(&st, &in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(is("core 1 free again: script ended after 52 s"));
    bgcheck_judge_core1(&st, &in, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
}

TEST(core1_stop_obeyed) {
    bgcheck_core1_state_t st = {0};
    bgcheck_core1_in_t in = {true, false, BGCHECK_WAIT_NONE, 1, S(0), 0, "x.lua"};
    bgcheck_judge_core1(&st, &in, &r);
    in.cancel = true;
    in.beats = 50;                              // the hook runs every instruction after a stop
    in.now_us = S(15);
    bgcheck_judge_core1(&st, &in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    in.beats = 90;
    in.now_us = S(30);
    bgcheck_judge_core1(&st, &in, &r);
    CHECK_EQ(r.status, HEALTH_PASS);            // still beating: stopping, not stuck
    in.running = false;
    bgcheck_judge_core1(&st, &in, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);            // never troubled: no "free again"
}

// --- supply's brownout detector ------------------------------------------------------------------------

TEST(bod) {
    r.status = HEALTH_PASS;
    snprintf(r.text, sizeof(r.text), "VSYS min 4.61 V since the last check, USB power");
    bgcheck_judge_bod(0x0b1, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(is("VSYS min 4.61 V since the last check, USB power; BOD on"));
    bgcheck_judge_bod(0x0b0, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(is("brownout detector off or moved (BOD 0x000000b0)"));
    r.status = HEALTH_FAIL;
    snprintf(r.text, sizeof(r.text), "VSYS min 2.90 V since the last check, no USB power");
    bgcheck_judge_bod(0x091, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);            // the supply's FAIL says more
    CHECK(says("2.90 V"));
    r.status = HEALTH_SKIP;
    snprintf(r.text, sizeof(r.text), "no VSYS reading");
    bgcheck_judge_bod(0x0b1, &r);
    CHECK(is("no VSYS reading"));
}

TEST_MAIN(
    RUN(resets_pass);
    RUN(resets_warn_reasons);
    RUN(resets_fault_record);
    RUN(resets_loop_first);
    RUN(resets_history_trouble);
    RUN(drift_arithmetic);
    RUN(drift_rtc_warn_uncalibrated);
    RUN(drift_rtc_warn_calibrated);
    RUN(drift_rtc_beyond_range);
    RUN(drift_rtc_pass);
    RUN(drift_rtc_too_short);
    RUN(drift_rtc_not_measured);
    RUN(drift_xosc);
    RUN(drift_no_rtc);
    RUN(heap_pass);
    RUN(heap_tags_over);
    RUN(heap_mallinfo_inconsistent);
    RUN(heap_end_past_guard);
    RUN(core1_paths);
    RUN(core1_stop_obeyed);
    RUN(bod);
)
