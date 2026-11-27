// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The pure half of phase 4's health checks, the background tier (core/health.h,
// docs/design/health.md §9.5): `resets` with every reason and the reset history, `rtc-drift`,
// `heap-sum`, `core1`, and the brownout detector's setting for `supply`. bgcheck_board.c reads
// the chip and the rest of SOS and hands what it found here; tests/test_bgcheck.c runs these with
// made-up inputs, one test per status path at least.
//
// None of these checks holds a gate, and none repairs anything.

#ifndef SOS_BGCHECK_H
#define SOS_BGCHECK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/health.h"
#include "core/history.h"
#include "core/resetwhy.h"
#include "time/timeconv.h"

// --- resets: the last reset's reason, and boot loops from the history -----------------------------

#define BGCHECK_LOOP_RESETS 3           // GLITCHDET_GUARD_RESETS: short runs in a row for a loop
#define BGCHECK_STABLE_S    180         // GLITCHDET_STABLE_S: a run shorter than this is short

typedef struct {
    resetwhy_t why;
    const char *text;                   // resetwhy_text(): a fault's record, or the name
    history_state_t history;            // the sector's state (after this boot's record)
    unsigned boots;                     // boot records in it
    unsigned damaged;                   // damaged and stray records
    unsigned short_runs;                // history_short_runs() over the newest boot records
} bgcheck_resets_t;

// WARN, in this order: a boot loop (BGCHECK_LOOP_RESETS short runs in a row); the last reset a
// fault, watchdog timeout, glitch detector, brownout, debugger or unknown; damaged records; the
// sector off (the image overlaps it) or of a newer format. PASS otherwise.
void bgcheck_judge_resets(const bgcheck_resets_t *in, health_result_t *out);

// --- rtc-drift ------------------------------------------------------------------------------------

#define BGCHECK_RTC_WARN_MPPM   20000   // a 32.768 kHz crystal's typical +-20 ppm
#define BGCHECK_XOSC_WARN_MPPM  65000   // ABM8-272-T3: 30 tolerance + 30 stability + 5 aging
#define BGCHECK_XOSC_FAIL_MPPM  1000000 // the RP2350's clock margin (datasheet §8.1.2.3)
// What `time cal` can set (timeconv_cal_steps()).
#define BGCHECK_CAL_MIN_MPPM    (TIMECONV_CAL_STEPS_MIN * TIMECONV_CAL_STEP_MPPM)
#define BGCHECK_CAL_MAX_MPPM    (TIMECONV_CAL_STEPS_MAX * TIMECONV_CAL_STEP_MPPM)

// Each part judged only when sure (timeconv_verdict()). RTC: WARN surely over 20 ppm, with the
// `time cal` value that would correct it, or beyond what `time cal` can do; PASS surely within;
// SKIP too short to judge, before a measurement, or after the calibration changed. XOSC: WARN
// surely over 65 ppm, FAIL surely over 1000 ppm. The worse part leads the text; the calibration in
// force is always named.
void bgcheck_judge_drift(const timeconv_drift_t *d, health_result_t *out);

// --- heap-sum: the heap's totals -------------------------------------------------------------------

typedef struct {
    size_t used, free, arena;           // mallinfo(): uordblks, fordblks, arena
    size_t tags;                        // the sum of heaptag's tags
} bgcheck_heap_read_t;

typedef struct {
    bgcheck_heap_read_t a, b;           // b: read again 1 ms later if a showed the tags over
    uintptr_t start;                    // __end__: where the heap starts
    uintptr_t end;                      // sbrk(0): where it ends now
    uintptr_t limit;                    // the lower of __HeapLimit and __StackLimit
    uintptr_t msplim;                   // core 0's stack guard (MSPLIM); 0 if none
} bgcheck_heap_t;

// FAIL: the heap's end past its limit or the stack guard. WARN: the tags' sum over the bytes in
// use in both readings (a release under the wrong tag, or twice); in use + free not the arena;
// the heap's end short of start + arena. PASS otherwise.
void bgcheck_judge_heap(const bgcheck_heap_t *h, health_result_t *out);

// --- core1: the heartbeat of a script on core 1 ------------------------------------------------------

#define BGCHECK_CORE1_WARN_US (30u * 1000000)   // no beat while not waiting: a long C call
#define BGCHECK_CORE1_STOP_US (10u * 1000000)   // no beat since a stop was asked: stuck

typedef enum { BGCHECK_WAIT_NONE, BGCHECK_WAIT_CORE0, BGCHECK_WAIT_OUTPUT, BGCHECK_WAIT_LINE }
    bgcheck_wait_t;

typedef struct {
    bool running;                       // a script is between start and done
    bool cancel;                        // a stop was asked (Ctrl-C, lua stop)
    bgcheck_wait_t waiting;
    uint32_t beats;                     // core 1's counter
    uint64_t now_us;
    uint32_t run_s;                     // how long the script has been running
    const char *name;                   // "loop.lua", "lua -e", "lua prompt"
} bgcheck_core1_in_t;

// Kept by the check between runs (zeroed at boot).
typedef struct {
    bool tracking;
    uint32_t beats;                     // at the last run
    uint64_t at_us;                     // the last run
    uint64_t moved_us;                  // when the beats last moved (or core 1 last waited)
    uint64_t stop_us;                   // when the stop was first seen; 0: none
    uint32_t run_s;
    bool troubled;                      // this script was WARNed or FAILed
} bgcheck_core1_state_t;

// PASS while beating or waiting; WARN after 30 s without a beat, not waiting and no stop asked;
// FAIL once a stop was asked 10 s ago and nothing has moved for 10 s; after a troubled script
// ends, PASS once ("core 1 free again"); SKIP with no script.
void bgcheck_judge_core1(bgcheck_core1_state_t *st, const bgcheck_core1_in_t *in,
                         health_result_t *out);

// --- supply: the brownout detector's setting ------------------------------------------------------

#define BGCHECK_BOD_MASK    0x1f1u      // VSEL (8:4) and EN (0)
#define BGCHECK_BOD_DEFAULT 0x0b1u      // 0.946 V, on: its reset value; nothing in SOS writes it

// After health_judge_supply(): WARN if the register isn't at its default (unless the supply is
// already WARN or FAIL); a PASS names the detector.
void bgcheck_judge_bod(uint32_t bod, health_result_t *out);

#endif
