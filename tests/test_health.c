// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/health.c, the health check framework (docs/design/health.md, #233):
// status changes and what is logged, gates, the boot tier, background scheduling, jobs and
// cancel, the text table, the Lua record, and the seed checks' judging. The log, the clock and
// lua_info.h's writer are stand-ins here (the writer records a trace of what was written).

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/health.h"
#include "core/log.h"
#include "core/sensors.h"
#include "test.h"

// --- stand-ins -----------------------------------------------------------------------------------

static uint64_t clock_now;
static uint64_t fake_clock(void) { return clock_now; }
static void advance_s(uint32_t s) { clock_now += (uint64_t)s * 1000000u; }

static char logged[16][192];
static int logged_level[16];
static int log_count;
void log_msg(int priority, const char *fmt, ...) {
    if (log_count >= 16)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(logged[log_count], sizeof(logged[0]), fmt, ap);
    va_end(ap);
    logged_level[log_count++] = priority;
}
static void log_reset(void) { log_count = 0; }

// The writer: each call appends "name=value " or "{name " / "} " to a trace.
static char trace[2048];
static void tr(const char *fmt, ...) {
    size_t len = strlen(trace);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(trace + len, sizeof(trace) - len, fmt, ap);
    va_end(ap);
}
void lua_info_begin(lua_info_t *w, uint8_t *buf, size_t cap) {
    memset(w, 0, sizeof(*w));
    w->buf = buf;
    w->cap = cap;
    trace[0] = '\0';
}
void lua_info_int(lua_info_t *w, const char *name, int64_t v) { (void)w; tr("%s=%lld ", name, (long long)v); }
void lua_info_num(lua_info_t *w, const char *name, double v) { (void)w; tr("%s=%g ", name, v); }
void lua_info_bool(lua_info_t *w, const char *name, bool v) { (void)w; tr("%s=%d ", name, v); }
void lua_info_str(lua_info_t *w, const char *name, const char *s) {
    (void)w;
    if (s)
        tr("%s=%s ", name, s);
}
void lua_info_open(lua_info_t *w, const char *name) { (void)w; tr("{%s ", name); }
void lua_info_close(lua_info_t *w) { (void)w; tr("} "); }

// --- test checks ---------------------------------------------------------------------------------

// A check whose result the test sets; counts its calls. `steps`: how many calls it takes.
typedef struct {
    health_status_t status;
    const char *text;
    int steps;
    int calls, cancels, frees;
} fake_t;

static fake_t fakes[6];

static health_step_t fake_step(int k, void **state, bool cancel, health_result_t *out) {
    fake_t *f = &fakes[k];
    f->calls++;
    if (cancel) {
        f->cancels++;
        if (*state) {
            free(*state);
            f->frees++;
            *state = NULL;
        }
        return HEALTH_DONE;
    }
    if (!*state) {
        *state = calloc(1, sizeof(int));
    }
    int *done = *state;
    if (++*done < f->steps)
        return HEALTH_MORE;
    free(*state);
    f->frees++;
    *state = NULL;
    out->status = f->status;
    snprintf(out->text, sizeof(out->text), "%s", f->text ? f->text : "ok");
    return HEALTH_DONE;
}

static health_step_t step0(void **s, bool c, health_result_t *o) { return fake_step(0, s, c, o); }
static health_step_t step1(void **s, bool c, health_result_t *o) { return fake_step(1, s, c, o); }
static health_step_t step2(void **s, bool c, health_result_t *o) { return fake_step(2, s, c, o); }
static health_step_t step3(void **s, bool c, health_result_t *o) { return fake_step(3, s, c, o); }
static health_step_t step4(void **s, bool c, health_result_t *o) { return fake_step(4, s, c, o); }
static health_step_t step5(void **s, bool c, health_result_t *o) { return fake_step(5, s, c, o); }

static const health_check_t table[] = {
    {"trng", HEALTH_BOOT, 0, 0, HEALTH_GATE_KEYS, step0},
    {"kat", HEALTH_BOOT, 0, 0, HEALTH_GATE_KEYS | HEALTH_GATE_CRYPTO, step1},
    {"stack", HEALTH_BACKGROUND, 60, 0, 0, step2},
    {"heap", HEALTH_BACKGROUND, 30, 0, 0, step3},
    {"walk", HEALTH_BACKGROUND, 60, 5, 0, step4},
    {"ram", HEALTH_FULL, 0, 0, 0, step5},
};
#define N (sizeof(table) / sizeof(table[0]))

static void setup(void) {
    memset(fakes, 0, sizeof(fakes));
    for (unsigned i = 0; i < N; i++) {
        fakes[i].status = HEALTH_PASS;
        fakes[i].steps = 1;
    }
    clock_now = 1000000;                        // 1 s after boot
    log_reset();
    health_init(table, N, fake_clock);
}

static health_status_t status_of(const char *name) {
    return health_entry((unsigned)health_find(name))->result.status;
}

// --- status changes and logging ------------------------------------------------------------------

TEST(steady_board_logs_only_the_summary) {
    setup();
    health_run_boot();
    CHECK_EQ(status_of("trng"), HEALTH_PASS);
    CHECK_EQ(status_of("stack"), HEALTH_NONE);   // not a boot check
    CHECK_EQ(log_count, 1);
    CHECK(!strcmp(logged[0], "health: boot checks 2 pass in 0 ms"));
    CHECK_EQ(logged_level[0], SOS_LOG_KERN | SOS_LOG_NOTICE);
    CHECK_EQ(health_entry(0)->runs, 1);
    CHECK(health_entry(0)->at_boot);
}

TEST(changes_are_logged) {
    setup();
    fakes[0].status = HEALTH_FAIL;
    fakes[0].text = "repetition count 41 > 40";
    fakes[1].status = HEALTH_SKIP;
    health_run_boot();
    CHECK_EQ(log_count, 2);
    CHECK(!strcmp(logged[0], "health: trng FAIL: repetition count 41 > 40"));
    CHECK_EQ(logged_level[0], SOS_LOG_KERN | SOS_LOG_ERR);
    CHECK(!strcmp(logged[1],
                  "health: boot checks 1 skip, 1 FAIL (trng) in 0 ms: key making refused"));
    CHECK_EQ(logged_level[1], SOS_LOG_KERN | SOS_LOG_WARNING);
    CHECK_EQ(health_entry(0)->fails, 1);

    // The same again: no line. Back to PASS: a notice that says for how long.
    log_reset();
    advance_s(180);
    CHECK(health_inject("trng", HEALTH_FAIL, "again"));
    CHECK_EQ(log_count, 0);
    CHECK(health_inject("trng", HEALTH_PASS, "fine"));
    CHECK_EQ(log_count, 1);
    CHECK(!strcmp(logged[0], "health: trng PASS (was FAIL 3 min ago)"));
    CHECK_EQ(logged_level[0], SOS_LOG_KERN | SOS_LOG_NOTICE);
    CHECK_EQ(health_entry(0)->runs, 3);
    CHECK_EQ(health_entry(0)->fails, 2);

    // To WARN: a warning; and the boot flag goes with a later result.
    log_reset();
    CHECK(health_inject("trng", HEALTH_WARN, "core 0 peak 6.9 of 8 KB"));
    CHECK(!strcmp(logged[0], "health: trng WARN: core 0 peak 6.9 of 8 KB"));
    CHECK_EQ(logged_level[0], SOS_LOG_KERN | SOS_LOG_WARNING);
    CHECK(!health_entry(0)->at_boot);
    CHECK(!health_inject("nope", HEALTH_FAIL, "x"));
}

TEST(boot_summary_names_warnings_and_both_gates) {
    setup();
    fakes[0].status = HEALTH_WARN;
    fakes[1].status = HEALTH_FAIL;
    health_run_boot();
    CHECK(!strcmp(logged[log_count - 1], "health: boot checks 1 WARN (trng), 1 FAIL (kat) in 0 ms: "
                                         "key making and encrypting refused"));
}

TEST(boot_tier_refuses_more) {
    setup();
    fakes[1].steps = 2;                         // would need a second call
    health_run_boot();
    CHECK_EQ(status_of("kat"), HEALTH_FAIL);
    CHECK_EQ(fakes[1].cancels, 1);
    CHECK_EQ(fakes[1].frees, 1);                // its state was freed by the cancel
    CHECK(!strcmp(health_entry(1)->result.text, "needed more than one step at boot (a bug)"));
}

TEST(boot_time_is_measured) {
    setup();
    health_run_boot();
    CHECK_EQ(health_boot_ms(), 0);
}

// --- gates ---------------------------------------------------------------------------------------

TEST(gates) {
    setup();
    CHECK(health_gate(HEALTH_GATE_KEYS) == NULL);       // nothing has run: open
    health_status_t open[] = {HEALTH_SKIP, HEALTH_PASS, HEALTH_WARN};
    for (unsigned i = 0; i < 3; i++) {
        health_inject("kat", open[i], "x");
        CHECK(health_gate(HEALTH_GATE_KEYS | HEALTH_GATE_CRYPTO) == NULL);
    }
    health_inject("kat", HEALTH_FAIL, "x");
    CHECK(!strcmp(health_gate(HEALTH_GATE_CRYPTO), "kat"));
    CHECK(!strcmp(health_gate(HEALTH_GATE_KEYS), "kat"));
    health_inject("trng", HEALTH_FAIL, "x");
    CHECK(!strcmp(health_gate(HEALTH_GATE_KEYS), "trng"));     // the first in table order
    CHECK(!strcmp(health_gate(HEALTH_GATE_CRYPTO), "kat"));    // trng doesn't hold CRYPTO
    // A later PASS opens it again (the latest result counts).
    health_inject("trng", HEALTH_PASS, "x");
    health_inject("kat", HEALTH_PASS, "x");
    CHECK(health_gate(HEALTH_GATE_KEYS | HEALTH_GATE_CRYPTO) == NULL);
    // A check added later holds its gate the same way.
    static const health_check_t extra = {"inj-keys", HEALTH_FULL, 0, 0, HEALTH_GATE_KEYS,
                                         step5};
    CHECK(health_add(&extra));
    CHECK(!health_add(&extra));                     // once
    CHECK(health_inject("inj-keys", HEALTH_FAIL, "by hand"));
    CHECK(!strcmp(health_gate(HEALTH_GATE_KEYS), "inj-keys"));
}

// --- background scheduling -----------------------------------------------------------------------

TEST(background_waits_for_its_period) {
    setup();
    health_poll();
    CHECK_EQ(fakes[2].calls + fakes[3].calls + fakes[4].calls, 0);
    advance_s(5);                               // walk's first run: its own shorter delay
    health_poll();
    CHECK_EQ(fakes[4].calls, 1);
    CHECK_EQ(status_of("walk"), HEALTH_PASS);
    health_poll();                              // nothing else due
    CHECK_EQ(fakes[2].calls + fakes[3].calls, 0);
}

TEST(most_overdue_first_one_per_poll) {
    setup();
    advance_s(120);                             // all three due: stack at 61, heap 31, walk 6
    health_poll();
    CHECK_EQ(fakes[4].calls, 1);                // walk: due first
    CHECK_EQ(fakes[3].calls, 0);
    health_poll();
    CHECK_EQ(fakes[3].calls, 1);                // then heap
    CHECK_EQ(fakes[2].calls, 0);
    health_poll();
    CHECK_EQ(fakes[2].calls, 1);                // then stack
    health_poll();
    CHECK_EQ(fakes[2].calls + fakes[3].calls + fakes[4].calls, 3);
    advance_s(30);                              // heap again (every 30 s), not the others
    health_poll();
    CHECK_EQ(fakes[3].calls, 2);
    CHECK_EQ(fakes[2].calls, 1);
    CHECK_EQ(fakes[4].calls, 1);
}

TEST(more_resumes_on_later_polls) {
    setup();
    fakes[4].steps = 3;
    advance_s(5);
    health_poll();
    health_poll();
    CHECK_EQ(status_of("walk"), HEALTH_NONE);
    advance_s(100);                             // others are due, but walk is part-way through
    health_poll();
    CHECK_EQ(fakes[4].calls, 3);
    CHECK_EQ(status_of("walk"), HEALTH_PASS);
    CHECK_EQ(fakes[2].calls + fakes[3].calls, 0);
    CHECK_EQ(fakes[4].frees, 1);
}

// --- jobs ----------------------------------------------------------------------------------------

static int done_order[16], done_count;
static void on_done(unsigned i, void *arg) {
    (void)arg;
    done_order[done_count++] = (int)i;
}

TEST(run_job_covers_boot_and_background) {
    setup();
    done_count = 0;
    CHECK(health_job_start(HEALTH_JOB_RUN, NULL, on_done, NULL) == NULL);
    CHECK(health_job_running());
    CHECK(!strcmp(health_job_start(HEALTH_JOB_RUN, NULL, on_done, NULL),
                  "checks are already running"));
    while (health_job_step(1000000, false))
        ;
    CHECK(!health_job_running());
    CHECK_EQ(done_count, 5);                    // not ram (full tier)
    for (int i = 0; i < 5; i++)
        CHECK_EQ(done_order[i], i);
    CHECK_EQ(status_of("ram"), HEALTH_NONE);
    CHECK(!health_entry(0)->at_boot);
}

TEST(full_job_and_one_check) {
    setup();
    done_count = 0;
    CHECK(health_job_start(HEALTH_JOB_FULL, NULL, on_done, NULL) == NULL);
    while (health_job_step(1000000, false))
        ;
    CHECK_EQ(done_count, 6);
    CHECK_EQ(status_of("ram"), HEALTH_PASS);

    done_count = 0;
    CHECK(!strcmp(health_job_start(HEALTH_JOB_RUN, "nope", on_done, NULL), "no check named nope"));
    CHECK(health_job_start(HEALTH_JOB_RUN, "ram", on_done, NULL) == NULL);   // any tier by name
    while (health_job_step(1000000, false))
        ;
    CHECK_EQ(done_count, 1);
    CHECK_EQ(done_order[0], 5);
    CHECK_EQ(health_entry(5)->runs, 2);
}

TEST(job_budget_and_multi_step) {
    setup();
    fakes[5].steps = 4;
    done_count = 0;
    CHECK(health_job_start(HEALTH_JOB_RUN, "ram", on_done, NULL) == NULL);
    int calls = 0;
    while (health_job_step(0, false))           // no budget: one step per call
        calls++;
    CHECK_EQ(calls, 4);                         // the last says there's more until it looks
    CHECK_EQ(fakes[5].calls, 4);
    CHECK_EQ(done_count, 1);
}

TEST(nothing_polls_while_a_job_runs) {
    setup();
    fakes[5].steps = 3;
    advance_s(200);
    CHECK(health_job_start(HEALTH_JOB_RUN, "ram", NULL, NULL) == NULL);
    health_job_step(0, false);
    health_poll();
    health_poll();
    CHECK_EQ(fakes[2].calls + fakes[3].calls + fakes[4].calls, 0);
    while (health_job_step(0, false))
        ;
    health_poll();
    CHECK_EQ(fakes[4].calls, 1);
}

TEST(cancel_frees_state_and_keeps_results) {
    setup();
    health_run_boot();
    fakes[5].steps = 5;
    fakes[0].status = HEALTH_WARN;
    CHECK(health_job_start(HEALTH_JOB_FULL, NULL, NULL, NULL) == NULL);
    while (fakes[5].calls < 2)
        health_job_step(0, false);
    CHECK(!health_job_step(0, true));           // Ctrl-C
    CHECK(!health_job_running());
    CHECK_EQ(fakes[5].cancels, 1);
    CHECK_EQ(fakes[5].frees, 1);
    CHECK_EQ(status_of("ram"), HEALTH_NONE);    // cancelled: no result
    CHECK_EQ(status_of("trng"), HEALTH_WARN);   // finished before: kept
}

TEST(job_cancels_a_background_check_part_way) {
    setup();
    fakes[4].steps = 3;
    advance_s(5);
    health_poll();                              // walk starts
    CHECK(health_job_start(HEALTH_JOB_RUN, "trng", NULL, NULL) == NULL);
    CHECK_EQ(fakes[4].cancels, 1);
    CHECK_EQ(fakes[4].frees, 1);
    while (health_job_step(0, false))
        ;
    health_poll();                              // walk starts over, from a fresh state
    CHECK_EQ(fakes[4].calls, 3);
    CHECK_EQ(status_of("walk"), HEALTH_NONE);
}

// --- text and Lua --------------------------------------------------------------------------------

TEST(text_table) {
    setup();
    fakes[0].text = "repetition 3/40";
    health_run_boot();
    advance_s(12);
    fakes[2].status = HEALTH_WARN;
    fakes[2].text = "core 0 peak 6.9 of 8 KB";
    health_job_start(HEALTH_JOB_RUN, "stack", NULL, NULL);
    while (health_job_step(1000000, false))
        ;
    advance_s(12);
    char row[HEALTH_ROW_MAX];
    health_format_header(row, sizeof(row));
    CHECK(!strcmp(row, "check       status  when       result"));
    health_format_row(row, sizeof(row), 0);
    CHECK(!strcmp(row, "trng        PASS    boot       repetition 3/40"));
    health_format_row(row, sizeof(row), 2);
    CHECK(!strcmp(row, "stack       WARN    12 s ago   core 0 peak 6.9 of 8 KB"));
    health_format_row(row, sizeof(row), 3);
    CHECK(!strcmp(row, "heap        -       never      (not run yet)"));
    health_format_row(row, sizeof(row), 5);
    CHECK(!strcmp(row, "ram         -       never      (health full)"));
    health_format_row(row, sizeof(row), 99);
    CHECK(!strcmp(row, ""));
    advance_s(3600);
    health_format_row(row, sizeof(row), 2);
    CHECK(!strncmp(row, "stack       WARN    1 h ago ", 27));
    // Cut short in a small buffer, still terminated.
    char small[10];
    health_format_row(small, sizeof(small), 2);
    CHECK_EQ(strlen(small), 9);
}

TEST(lua_record) {
    setup();
    health_run_boot();
    advance_s(7);
    health_inject("heap", HEALTH_WARN, "40 KB free");
    advance_s(3);
    lua_info_t w;
    uint8_t buf[16];
    lua_info_begin(&w, buf, sizeof(buf));
    health_lua_table(&w);
    CHECK(!strcmp(trace,
        "worst=warn boot_ms=0 {checks "
        "{ name=trng status=pass tier=boot text=ok ago_s=10 runs=1 fails=0 } "
        "{ name=kat status=pass tier=boot text=ok ago_s=10 runs=1 fails=0 } "
        "{ name=stack status=none tier=background runs=0 fails=0 } "
        "{ name=heap status=warn tier=background text=40 KB free ago_s=3 runs=1 fails=0 } "
        "{ name=walk status=none tier=background runs=0 fails=0 } "
        "{ name=ram status=none tier=full runs=0 fails=0 } } "));
    CHECK_EQ(health_worst(), HEALTH_WARN);
    health_inject("heap", HEALTH_SKIP, "x");
    CHECK_EQ(health_worst(), HEALTH_PASS);      // SKIP and NONE count as pass
}

// --- the seed checks' judging --------------------------------------------------------------------

TEST(judge_glitch) {
    health_result_t r;
    health_judge_glitch(false, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    health_judge_glitch(true, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
}

TEST(judge_stack) {
    health_result_t r;
    // 75 % of 8192 is 6144: at it PASS, one over WARN; 90 % is 7372.8.
    health_judge_stack(6144, 8192, 0, 32768, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(!strcmp(r.text, "core 0 6.0 of 8.0 KB"));
    health_judge_stack(6145, 8192, 0, 32768, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    health_judge_stack(7372, 8192, 0, 32768, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    health_judge_stack(7373, 8192, 0, 32768, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    // Core 1: the same lines of its 32 KB.
    health_judge_stack(1024, 8192, 24576, 32768, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(!strcmp(r.text, "core 0 1.0 of 8.0 KB, core 1 24.0 of 32.0 KB"));
    health_judge_stack(1024, 8192, 24577, 32768, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    health_judge_stack(1024, 8192, 29492, 32768, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    health_judge_stack(0, 0, 0, 0, &r);         // sizes unknown: nothing to judge against
    CHECK_EQ(r.status, HEALTH_PASS);
}

TEST(judge_heap_with_hysteresis) {
    health_result_t r;
    health_judge_heap(64 * 1024, HEALTH_NONE, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(!strcmp(r.text, "64 KB free"));
    health_judge_heap(64 * 1024 - 1, HEALTH_PASS, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    // The band (64 to 96 KB): stays as it was.
    health_judge_heap(80 * 1024, HEALTH_PASS, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    health_judge_heap(80 * 1024, HEALTH_WARN, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    health_judge_heap(96 * 1024, HEALTH_WARN, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    health_judge_heap(96 * 1024 + 1, HEALTH_WARN, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    health_judge_heap(80 * 1024, HEALTH_FAIL, &r);   // out of FAIL into the band: WARN
    CHECK_EQ(r.status, HEALTH_WARN);
    health_judge_heap(16 * 1024, HEALTH_WARN, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    health_judge_heap(16 * 1024 - 1, HEALTH_PASS, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
}

TEST(judge_temp) {
    health_result_t r;
    health_judge_temp(SENSORS_NONE, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
    health_judge_temp(312, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(!strcmp(r.text, "chip 31.2 C"));
    health_judge_temp(700, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    health_judge_temp(701, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    health_judge_temp(850, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    health_judge_temp(851, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    health_judge_temp(-5, &r);
    CHECK(!strcmp(r.text, "chip -0.5 C"));
    health_judge_temp(-123, &r);
    CHECK(!strcmp(r.text, "chip -12.3 C"));
}

TEST(judge_supply) {
    health_result_t r;
    health_judge_supply(SENSORS_NONE, 1, &r);
    CHECK_EQ(r.status, HEALTH_SKIP);
    health_judge_supply(4650, 1, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(!strcmp(r.text, "VSYS min 4.65 V since the last check, USB power"));
    health_judge_supply(4300, 1, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    health_judge_supply(4299, 1, &r);
    CHECK_EQ(r.status, HEALTH_WARN);
    CHECK(!strcmp(r.text, "VSYS min 4.30 V since the last check, USB power"));
    health_judge_supply(3700, 0, &r);           // a battery: 4.3 V doesn't apply
    CHECK_EQ(r.status, HEALTH_PASS);
    CHECK(!strcmp(r.text, "VSYS min 3.70 V since the last check, no USB power"));
    health_judge_supply(3700, -1, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    health_judge_supply(3000, 0, &r);
    CHECK_EQ(r.status, HEALTH_PASS);
    health_judge_supply(2999, 0, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    health_judge_supply(2999, 1, &r);
    CHECK_EQ(r.status, HEALTH_FAIL);
    health_judge_supply(4999, 1, &r);
    CHECK(!strcmp(r.text, "VSYS min 5.00 V since the last check, USB power"));
}

TEST_MAIN(
    RUN(steady_board_logs_only_the_summary);
    RUN(changes_are_logged);
    RUN(boot_summary_names_warnings_and_both_gates);
    RUN(boot_tier_refuses_more);
    RUN(boot_time_is_measured);
    RUN(gates);
    RUN(background_waits_for_its_period);
    RUN(most_overdue_first_one_per_poll);
    RUN(more_resumes_on_later_polls);
    RUN(run_job_covers_boot_and_background);
    RUN(full_job_and_one_check);
    RUN(job_budget_and_multi_step);
    RUN(nothing_polls_while_a_job_runs);
    RUN(cancel_frees_state_and_keeps_results);
    RUN(job_cancels_a_background_check_part_way);
    RUN(text_table);
    RUN(lua_record);
    RUN(judge_glitch);
    RUN(judge_stack);
    RUN(judge_heap_with_hysteresis);
    RUN(judge_temp);
    RUN(judge_supply);
)
