// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Self-checks of the board's hardware and stored state: `health` (docs/design/health.md, #233).
//
// Each check reports PASS, WARN or FAIL with a one-line reason. A FAIL is logged and closes the
// check's gates: the operations that depend on it (making keys, encrypting new data) ask
// health_gate() first and refuse while it is closed. Nothing stops the boot.
//
// health.c is pure C (host tested): the registry, the results, the scheduling, the gates, the
// text and the Lua record, and the functions that judge the seed checks' numbers. It reads the
// clock through the hook given to health_init(). health_board.c has the firmware's table of
// checks, which read the chip and the rest of SOS.
//
// Everything runs on core 0: the boot tier from main(), the background tier from the main loop
// (health_poll), `health run` and `health full` as a shell job, and Lua's sos.health() through
// core 0's LR_INFO answer.

#ifndef SOS_HEALTH_H
#define SOS_HEALTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lua/lua_info.h"

#define HEALTH_TEXT_MAX  64             // a result's text, with its terminator
#define HEALTH_NAME_MAX  12             // a check's name: [a-z0-9-]{1,12}
#define HEALTH_MAX       24             // checks in the registry, test-only ones included

typedef enum {
    HEALTH_NONE,    // not run yet
    HEALTH_SKIP,    // doesn't apply on this board (e.g. no VSYS reading)
    HEALTH_PASS,
    HEALTH_WARN,    // worth knowing; nothing is refused
    HEALTH_FAIL,    // refuses the check's gates
} health_status_t;

typedef enum {
    HEALTH_BOOT,        // once during boot, then on `health run`
    HEALTH_BACKGROUND,  // every period_s from the main loop, and on `health run`
    HEALTH_FULL,        // only on `health full` (console)
} health_tier_t;

// What a check function returns for one call.
typedef enum { HEALTH_DONE, HEALTH_MORE } health_step_t;

typedef struct {
    health_status_t status;
    char text[HEALTH_TEXT_MAX];     // one line, no newline: "core 0 peak 6.9 of 8 KB"
} health_result_t;

// --- gates ---------------------------------------------------------------------------------------

#define HEALTH_GATE_KEYS    (1u << 0)   // making a key or secret from random numbers
#define HEALTH_GATE_CRYPTO  (1u << 1)   // encrypting new data (sealing, encrypting a card)

typedef struct health_check {
    const char *name;
    health_tier_t tier;
    uint32_t period_s;              // HEALTH_BACKGROUND: how often
    uint32_t first_s;               // HEALTH_BACKGROUND: first run after boot; 0 = period_s
    uint32_t gates;                 // HEALTH_GATE_* bits refused while this check FAILs
    // One step of the check. *state is NULL on the first call and is the check's to use; a
    // check that needs more than one step returns HEALTH_MORE and is called again on a later
    // pass. With cancel set it must free what *state holds and return HEALTH_DONE. Fills *out
    // when it returns HEALTH_DONE without cancel. A step stays under 10 ms (it runs in the main
    // loop, which also serves SSH and the console). A boot check finishes in one step.
    health_step_t (*step)(void **state, bool cancel, health_result_t *out);
} health_check_t;

// NULL if the gate (one or more HEALTH_GATE_* bits) is open, else the name of the first
// FAILed check that holds it. Every gate is open until a check that holds it has failed: NONE,
// SKIP, PASS and WARN don't close it. A caller that refuses logs the refusal, naming the check.
const char *health_gate(uint32_t gate);

// --- the framework -------------------------------------------------------------------------------

// The microseconds since boot (time_us_64 on the board).
typedef uint64_t (*health_clock_fn)(void);

// Register the table (kept, not copied; at most HEALTH_MAX entries) and forget all results.
void health_init(const health_check_t *checks, unsigned n, health_clock_fn now_us);

// Add one check after health_init() (the test-only gate checks of `health inject gate`). False
// if the registry is full or a check of that name is there already.
bool health_add(const health_check_t *check);

// Run the boot tier: every HEALTH_BOOT check, once each, in table order. A boot check that
// returns HEALTH_MORE is cancelled and recorded as FAIL (a check at boot must finish in one
// call). Logs one summary line at the end, and keeps the tier's time for health_boot_ms().
void health_run_boot(void);
uint32_t health_boot_ms(void);

// From the main loop, once per pass: at most one step of one background check. A check that
// returned HEALTH_MORE is stepped again first; otherwise the most overdue check whose time has
// come is started. Does nothing while a job (below) runs.
void health_poll(void);

// --- jobs: `health run` and `health full` --------------------------------------------------------

typedef enum {
    HEALTH_JOB_RUN,         // the boot and background tiers (or the one named check)
    HEALTH_JOB_FULL,        // every check of every tier
} health_job_kind_t;

// Called for each check the job finishes, with its index (health_count order).
typedef void (*health_job_done_fn)(unsigned index, void *arg);

// Start a job. `name`: only that check, of any tier (HEALTH_JOB_RUN only); NULL for all of the
// kind's. A background check that is part-way through is cancelled first (it keeps its last
// result), so no check is stepped from two places. Returns NULL, or why the job can't start
// ("no check named ...", "checks are already running").
const char *health_job_start(health_job_kind_t kind, const char *name, health_job_done_fn done,
                             void *arg);

// One call of the job: steps checks in table order until budget_us has passed. True while there
// is more to do. With cancel, the check being run is cancelled (its old result stays), and the
// job ends: false. Checks finished before keep their new results.
bool health_job_step(uint32_t budget_us, bool cancel);
bool health_job_running(void);

// --- results -------------------------------------------------------------------------------------

typedef struct {
    const health_check_t *check;
    health_result_t result;         // status HEALTH_NONE until it has run
    uint32_t at_s;                  // uptime when the result was taken
    bool at_boot;                   // taken by the boot tier
    uint32_t runs, fails;           // completed runs, and those that FAILed
} health_entry_t;

unsigned health_count(void);
const health_entry_t *health_entry(unsigned index);     // NULL past the end
int health_find(const char *name);                      // index, or -1

// The worst status of any check: PASS when none is WARN or FAIL (NONE and SKIP count as PASS).
health_status_t health_worst(void);

// "none", "skip", "pass", "warn", "fail"; and the tiers' "boot", "background", "full".
const char *health_status_name(health_status_t s);
const char *health_tier_name(health_tier_t t);

// Set a check's result by hand, as if it had run (`health inject`, test builds). The change is
// logged as any other. False if there is no such check.
bool health_inject(const char *name, health_status_t status, const char *text);

// --- text: the `health` command's table ----------------------------------------------------------

#define HEALTH_ROW_MAX (HEALTH_TEXT_MAX + 40)

// The header line, and one check's line, as `health` prints them (no newline):
//   check       status  when       result
//   stack       WARN    12 s ago   core 0 peak 6.9 of 8 KB
// Always terminated.
void health_format_header(char *out, size_t cap);
void health_format_row(char *out, size_t cap, unsigned index);

// --- the Lua record: sos.health() ----------------------------------------------------------------

// Written with lua_info.h's writer (a pure record format: no Lua here): {worst=, boot_ms=,
// checks={ {name=, status=, tier=, text=, ago_s=, runs=, fails=}, ... }}. A check that hasn't
// run has status "none" and no ago_s.
void health_lua_table(lua_info_t *w);

// --- judging the seed checks (pure; health_board.c reads the numbers) ----------------------------

// resets: WARN if the last reset was a fault (with its record), the glitch detector or the
// watchdog.
void health_judge_resets(const char *fault, bool glitch, bool watchdog, health_result_t *out);

// glitch: WARN if the loop guard stopped the detector being armed at this boot.
void health_judge_glitch(bool guard_tripped, health_result_t *out);

// stack: the deeper of core 0 (peak of size) and core 1 (the Lua stack; peak 0 = no script
// yet). WARN over HEALTH_STACK_WARN_PCT, FAIL over HEALTH_STACK_FAIL_PCT.
#define HEALTH_STACK_WARN_PCT 75
#define HEALTH_STACK_FAIL_PCT 90
void health_judge_stack(size_t c0_peak, size_t c0_size, size_t c1_peak, size_t c1_size,
                        health_result_t *out);

// heap: FAIL under HEALTH_HEAP_FAIL free; WARN under HEALTH_HEAP_WARN, and back to PASS only
// above HEALTH_HEAP_CLEAR (hysteresis: ordinary use, two SFTP transfers at once, comes near the
// WARN line, and the check would flip with each). `was`: the check's previous status.
#define HEALTH_HEAP_FAIL  (16u * 1024)
#define HEALTH_HEAP_WARN  (64u * 1024)  // a TLS handshake needs about this much
#define HEALTH_HEAP_CLEAR (96u * 1024)
void health_judge_heap(size_t free_bytes, health_status_t was, health_result_t *out);

// chip-temp: tenths of a degree (SENSORS_NONE: SKIP). WARN over HEALTH_TEMP_WARN, FAIL over
// HEALTH_TEMP_FAIL. The sensor reads the die, a few degrees off when uncalibrated.
#define HEALTH_TEMP_WARN 700            // 70.0 C
#define HEALTH_TEMP_FAIL 850            // 85.0 C: the top of the chip's rated range
void health_judge_temp(int tenths, health_result_t *out);

// supply: the lowest VSYS (mV) since the last check, SENSORS_NONE for none (SKIP); usb as
// sensors_usb_power(). FAIL under HEALTH_VSYS_FAIL; WARN under HEALTH_VSYS_USB_WARN on USB
// power (about 4.65 V there; a Wi-Fi transmission dips it about 0.13 V).
#define HEALTH_VSYS_FAIL     3000
#define HEALTH_VSYS_USB_WARN 4300
void health_judge_supply(int vsys_min_mv, int usb, health_result_t *out);

// --- the board (health_board.c) ------------------------------------------------------------------

// In main(), after wolfCrypt_Init() and before anything makes a key: register the firmware's
// checks and run the boot tier.
void health_boot(void);

#ifdef SOS_TEST_COMMANDS
// `health inject gate keys|crypto` (test builds): add a test-only check holding that gate and
// set it to FAIL. False if the gate's name is unknown or the registry is full.
bool health_board_inject_gate(const char *gate);
#endif

#endif
