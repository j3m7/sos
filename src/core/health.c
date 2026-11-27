// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The health check framework (core/health.h): pure C, tested on the host (tests/test_health.c).
// The firmware's checks are in health_board.c.
//
// One check is stepped at a time, from one place: the boot tier from main(), then either the
// main loop (health_poll) or a shell job (health_job_step), never both: starting a job cancels a
// background check that is part-way through, and health_poll() starts nothing while a job runs.
// So a check's *state has one owner.

#include "core/health.h"

#include <stdio.h>
#include <string.h>

#include "core/log.h"
#include "core/sensors.h"

static health_entry_t entries[HEALTH_MAX];
static void *states[HEALTH_MAX];        // each check's *state while it is part-way through
static uint64_t due_us[HEALTH_MAX];     // background: when it next runs
static uint32_t changed_s[HEALTH_MAX];  // uptime of its last change of status
static unsigned count;
static health_clock_fn clock_us;
static uint32_t boot_ms;
static int busy = -1;                   // the background check part-way through, or -1

static struct {
    bool active;
    health_job_kind_t kind;
    int only;                           // the one check to run, or -1
    unsigned next;                      // the next index to look at
    int current;                        // the check being stepped, or -1
    health_job_done_fn done;
    void *arg;
} job;

static uint64_t now_us(void) {
    return clock_us ? clock_us() : 0;
}

static uint32_t now_s(void) {
    return (uint32_t)(now_us() / 1000000u);
}

// --- names and text --------------------------------------------------------------------------

const char *health_status_name(health_status_t s) {
    switch (s) {
    case HEALTH_NONE: return "none";
    case HEALTH_SKIP: return "skip";
    case HEALTH_PASS: return "pass";
    case HEALTH_WARN: return "warn";
    case HEALTH_FAIL: return "fail";
    }
    return "?";
}

static const char *status_upper(health_status_t s) {
    switch (s) {
    case HEALTH_NONE: return "-";
    case HEALTH_SKIP: return "SKIP";
    case HEALTH_PASS: return "PASS";
    case HEALTH_WARN: return "WARN";
    case HEALTH_FAIL: return "FAIL";
    }
    return "?";
}

const char *health_tier_name(health_tier_t t) {
    switch (t) {
    case HEALTH_BOOT:       return "boot";
    case HEALTH_BACKGROUND: return "background";
    case HEALTH_FULL:       return "full";
    }
    return "?";
}

// "45 s", "12 min", "3 h", "2 d": short, for the table's 10 columns and the log.
static void format_age(char *out, size_t cap, uint32_t s) {
    if (s < 60)
        snprintf(out, cap, "%u s", (unsigned)s);
    else if (s < 3600)
        snprintf(out, cap, "%u min", (unsigned)(s / 60));
    else if (s < 86400)
        snprintf(out, cap, "%u h", (unsigned)(s / 3600));
    else
        snprintf(out, cap, "%u d", (unsigned)(s / 86400));
}

// --- results ---------------------------------------------------------------------------------

// Store a result for check i and log a change of status. NONE to PASS or SKIP is not logged,
// so a steady board logs nothing.
static void record(unsigned i, const health_result_t *r, bool at_boot) {
    health_entry_t *e = &entries[i];
    health_status_t was = e->result.status;
    uint32_t now = now_s();
    e->result = *r;
    e->result.text[HEALTH_TEXT_MAX - 1] = '\0';
    e->at_s = now;
    e->at_boot = at_boot;
    e->runs++;
    if (r->status == HEALTH_FAIL)
        e->fails++;
    if (r->status == was)
        return;
    uint32_t was_for = now - changed_s[i];
    changed_s[i] = now;
    const char *name = e->check->name, *text = e->result.text;
    if (r->status == HEALTH_FAIL) {
        log_msg(SOS_LOG_KERN | SOS_LOG_ERR, "health: %s FAIL: %s", name, text);
    } else if (r->status == HEALTH_WARN) {
        log_msg(SOS_LOG_KERN | SOS_LOG_WARNING, "health: %s WARN: %s", name, text);
    } else if (was == HEALTH_WARN || was == HEALTH_FAIL) {
        char age[16];
        format_age(age, sizeof(age), was_for);
        log_msg(SOS_LOG_KERN | SOS_LOG_NOTICE, "health: %s %s (was %s %s ago)", name,
                status_upper(r->status), status_upper(was), age);
    }
}

static void cancel(unsigned i);

static void set_result(health_result_t *out, health_status_t s, const char *text) {
    out->status = s;
    snprintf(out->text, sizeof(out->text), "%s", text);
}

static void schedule(unsigned i, uint32_t after_s) {
    due_us[i] = now_us() + (uint64_t)after_s * 1000000u;
}

static uint32_t first_delay(const health_check_t *c) {
    return c->first_s ? c->first_s : c->period_s;
}

void health_init(const health_check_t *checks, unsigned n, health_clock_fn now) {
    // A check left part-way (a second init: the tests) frees what it holds first.
    if (busy >= 0)
        cancel((unsigned)busy);
    if (job.active && job.current >= 0)
        cancel((unsigned)job.current);
    memset(entries, 0, sizeof(entries));
    for (unsigned i = 0; i < HEALTH_MAX; i++)
        states[i] = NULL;
    memset(due_us, 0, sizeof(due_us));
    memset(changed_s, 0, sizeof(changed_s));
    memset(&job, 0, sizeof(job));
    clock_us = now;
    boot_ms = 0;
    busy = -1;
    count = n < HEALTH_MAX ? n : HEALTH_MAX;
    for (unsigned i = 0; i < count; i++) {
        entries[i].check = &checks[i];
        schedule(i, first_delay(&checks[i]));
    }
}

bool health_add(const health_check_t *check) {
    if (count >= HEALTH_MAX || health_find(check->name) >= 0)
        return false;
    entries[count].check = check;
    schedule(count, first_delay(check));
    count++;
    return true;
}

unsigned health_count(void) {
    return count;
}

const health_entry_t *health_entry(unsigned index) {
    return index < count ? &entries[index] : NULL;
}

int health_find(const char *name) {
    for (unsigned i = 0; i < count; i++)
        if (!strcmp(entries[i].check->name, name))
            return (int)i;
    return -1;
}

health_status_t health_worst(void) {
    health_status_t worst = HEALTH_PASS;
    for (unsigned i = 0; i < count; i++)
        if (entries[i].result.status > worst)
            worst = entries[i].result.status;
    return worst;
}

bool health_inject(const char *name, health_status_t status, const char *text) {
    int i = health_find(name);
    if (i < 0)
        return false;
    health_result_t r;
    set_result(&r, status, text);
    record((unsigned)i, &r, false);
    return true;
}

// --- gates -----------------------------------------------------------------------------------

const char *health_gate(uint32_t gate) {
    for (unsigned i = 0; i < count; i++)
        if ((entries[i].check->gates & gate) && entries[i].result.status == HEALTH_FAIL)
            return entries[i].check->name;
    return NULL;
}

// --- boot tier -------------------------------------------------------------------------------

// Which operations the failed checks' gates refuse, for the boot summary.
static const char *refused_text(uint32_t gates) {
    if ((gates & HEALTH_GATE_KEYS) && (gates & HEALTH_GATE_CRYPTO))
        return ": key making and encrypting refused";
    if (gates & HEALTH_GATE_KEYS)
        return ": key making refused";
    if (gates & HEALTH_GATE_CRYPTO)
        return ": encrypting refused";
    return "";
}

// "1 FAIL (trng, otp)": the names of the checks at that status, as many as fit.
static void add_count(char *out, size_t cap, unsigned n, health_status_t s, bool names) {
    if (!n)
        return;
    size_t len = strlen(out);
    snprintf(out + len, cap - len, "%s%u %s", len ? ", " : "", n,
             names ? status_upper(s) : health_status_name(s));
    if (!names)
        return;
    bool first = true;
    for (unsigned i = 0; i < count; i++) {
        if (entries[i].check->tier != HEALTH_BOOT || entries[i].result.status != s)
            continue;
        len = strlen(out);
        snprintf(out + len, cap - len, "%s%s", first ? " (" : ", ", entries[i].check->name);
        first = false;
    }
    len = strlen(out);
    snprintf(out + len, cap - len, ")");
}

void health_run_boot(void) {
    uint64_t t0 = now_us();
    unsigned n[HEALTH_FAIL + 1] = {0};
    uint32_t gates = 0;
    for (unsigned i = 0; i < count; i++) {
        const health_check_t *c = entries[i].check;
        if (c->tier != HEALTH_BOOT)
            continue;
        health_result_t r;
        set_result(&r, HEALTH_NONE, "");
        void *state = NULL;
        if (c->step(&state, false, &r) == HEALTH_MORE) {
            health_result_t ignored;
            c->step(&state, true, &ignored);
            set_result(&r, HEALTH_FAIL, "needed more than one step at boot (a bug)");
        }
        record(i, &r, true);
        n[r.status]++;
        if (r.status == HEALTH_FAIL)
            gates |= c->gates;
    }
    boot_ms = (uint32_t)((now_us() - t0) / 1000u);
    char counts[HEALTH_ROW_MAX] = "";
    add_count(counts, sizeof(counts), n[HEALTH_PASS], HEALTH_PASS, false);
    add_count(counts, sizeof(counts), n[HEALTH_SKIP], HEALTH_SKIP, false);
    add_count(counts, sizeof(counts), n[HEALTH_WARN], HEALTH_WARN, true);
    add_count(counts, sizeof(counts), n[HEALTH_FAIL], HEALTH_FAIL, true);
    bool bad = n[HEALTH_WARN] || n[HEALTH_FAIL];
    log_msg(SOS_LOG_KERN | (bad ? SOS_LOG_WARNING : SOS_LOG_NOTICE),
            "health: boot checks %s in %u ms%s", counts[0] ? counts : "none",
            (unsigned)boot_ms, refused_text(gates));
}

uint32_t health_boot_ms(void) {
    return boot_ms;
}

// --- background tier -------------------------------------------------------------------------

// Step check i once; true when it has finished (its result recorded).
static bool step_once(unsigned i) {
    const health_check_t *c = entries[i].check;
    health_result_t r;
    set_result(&r, HEALTH_NONE, "");
    if (c->step(&states[i], false, &r) == HEALTH_MORE)
        return false;
    states[i] = NULL;
    record(i, &r, false);
    if (c->tier == HEALTH_BACKGROUND)
        schedule(i, c->period_s);
    return true;
}

static void cancel(unsigned i) {
    health_result_t ignored;
    entries[i].check->step(&states[i], true, &ignored);
    states[i] = NULL;
}

void health_poll(void) {
    if (job.active)
        return;
    if (busy >= 0) {
        if (step_once((unsigned)busy))
            busy = -1;
        return;
    }
    uint64_t now = now_us();
    int pick = -1;
    for (unsigned i = 0; i < count; i++) {
        if (entries[i].check->tier != HEALTH_BACKGROUND || due_us[i] > now)
            continue;
        if (pick < 0 || due_us[i] < due_us[pick])
            pick = (int)i;
    }
    if (pick >= 0 && !step_once((unsigned)pick))
        busy = pick;
}

// --- jobs ------------------------------------------------------------------------------------

static bool in_job(unsigned i) {
    if (job.only >= 0)
        return (int)i == job.only;
    return job.kind == HEALTH_JOB_FULL || entries[i].check->tier != HEALTH_FULL;
}

const char *health_job_start(health_job_kind_t kind, const char *name, health_job_done_fn done,
                             void *arg) {
    static char why[32 + HEALTH_NAME_MAX];
    if (job.active)
        return "checks are already running";
    int only = -1;
    if (name) {
        only = health_find(name);
        if (only < 0) {
            snprintf(why, sizeof(why), "no check named %.*s", HEALTH_NAME_MAX * 2, name);
            return why;
        }
    }
    if (busy >= 0) {
        cancel((unsigned)busy);
        busy = -1;
    }
    job.active = true;
    job.kind = kind;
    job.only = only;
    job.next = 0;
    job.current = -1;
    job.done = done;
    job.arg = arg;
    return NULL;
}

bool health_job_step(uint32_t budget_us, bool cancelled) {
    if (!job.active)
        return false;
    if (cancelled) {
        if (job.current >= 0)
            cancel((unsigned)job.current);
        job.active = false;
        return false;
    }
    uint64_t t0 = now_us();
    for (;;) {
        if (job.current < 0) {
            while (job.next < count && !in_job(job.next))
                job.next++;
            if (job.next >= count) {
                job.active = false;
                return false;
            }
            job.current = (int)job.next++;
        }
        unsigned i = (unsigned)job.current;
        if (step_once(i)) {
            job.current = -1;
            if (job.done)
                job.done(i, job.arg);
        }
        if (now_us() - t0 >= budget_us)
            return true;
    }
}

bool health_job_running(void) {
    return job.active;
}

// --- text ------------------------------------------------------------------------------------

void health_format_header(char *out, size_t cap) {
    snprintf(out, cap, "%-11s %-7s %-10s %s", "check", "status", "when", "result");
}

void health_format_row(char *out, size_t cap, unsigned index) {
    const health_entry_t *e = health_entry(index);
    if (!e) {
        if (cap)
            out[0] = '\0';
        return;
    }
    char when[16];
    const char *text = e->result.text;
    if (e->result.status == HEALTH_NONE) {
        snprintf(when, sizeof(when), "never");
        text = e->check->tier == HEALTH_FULL ? "(health full)" : "(not run yet)";
    } else if (e->at_boot) {
        snprintf(when, sizeof(when), "boot");
    } else {
        char age[12];
        format_age(age, sizeof(age), now_s() - e->at_s);
        snprintf(when, sizeof(when), "%s ago", age);
    }
    snprintf(out, cap, "%-11s %-7s %-10s %s", e->check->name, status_upper(e->result.status),
             when, text);
}

// --- Lua -------------------------------------------------------------------------------------

void health_lua_table(lua_info_t *w) {
    health_status_t worst = health_worst();
    lua_info_str(w, "worst", health_status_name(worst));
    lua_info_int(w, "boot_ms", boot_ms);
    lua_info_open(w, "checks");
    uint32_t now = now_s();
    for (unsigned i = 0; i < count; i++) {
        const health_entry_t *e = &entries[i];
        lua_info_open(w, "");
        lua_info_str(w, "name", e->check->name);
        lua_info_str(w, "status", health_status_name(e->result.status));
        lua_info_str(w, "tier", health_tier_name(e->check->tier));
        if (e->result.status != HEALTH_NONE) {
            lua_info_str(w, "text", e->result.text);
            lua_info_int(w, "ago_s", now - e->at_s);
        }
        lua_info_int(w, "runs", e->runs);
        lua_info_int(w, "fails", e->fails);
        lua_info_close(w);
    }
    lua_info_close(w);
}

// --- judging the seed checks -----------------------------------------------------------------

void health_judge_glitch(bool guard_tripped, health_result_t *out) {
    if (guard_tripped)
        set_result(out, HEALTH_WARN, "loop guard tripped: detector left off (see glitch)");
    else
        set_result(out, HEALTH_PASS, "loop guard not tripped");
}

// Tenths of a KB, rounded.
static unsigned kb_tenths(size_t bytes) {
    return (unsigned)((bytes * 10u + 512u) / 1024u);
}

// Over pct percent of size?
static bool over(size_t peak, size_t size, unsigned pct) {
    return size && (uint64_t)peak * 100u > (uint64_t)size * pct;
}

void health_judge_stack(size_t c0_peak, size_t c0_size, size_t c1_peak, size_t c1_size,
                        health_result_t *out) {
    unsigned a = kb_tenths(c0_peak), b = kb_tenths(c0_size);
    int n = snprintf(out->text, sizeof(out->text), "core 0 %u.%u of %u.%u KB", a / 10, a % 10,
                     b / 10, b % 10);
    if (c1_peak && n > 0 && (size_t)n < sizeof(out->text)) {
        unsigned c = kb_tenths(c1_peak), d = kb_tenths(c1_size);
        snprintf(out->text + n, sizeof(out->text) - (size_t)n, ", core 1 %u.%u of %u.%u KB",
                 c / 10, c % 10, d / 10, d % 10);
    }
    if (over(c0_peak, c0_size, HEALTH_STACK_FAIL_PCT) ||
        over(c1_peak, c1_size, HEALTH_STACK_FAIL_PCT))
        out->status = HEALTH_FAIL;
    else if (over(c0_peak, c0_size, HEALTH_STACK_WARN_PCT) ||
             over(c1_peak, c1_size, HEALTH_STACK_WARN_PCT))
        out->status = HEALTH_WARN;
    else
        out->status = HEALTH_PASS;
}

void health_judge_heap(size_t free_bytes, health_status_t was, health_result_t *out) {
    snprintf(out->text, sizeof(out->text), "%u KB free", (unsigned)(free_bytes / 1024u));
    if (free_bytes < HEALTH_HEAP_FAIL)
        out->status = HEALTH_FAIL;
    else if (free_bytes < HEALTH_HEAP_WARN)
        out->status = HEALTH_WARN;
    else if ((was == HEALTH_WARN || was == HEALTH_FAIL) && free_bytes <= HEALTH_HEAP_CLEAR)
        out->status = HEALTH_WARN;      // in the band: stays WARN until it has room again
    else
        out->status = HEALTH_PASS;
}

void health_judge_temp(int tenths, health_result_t *out) {
    if (tenths == SENSORS_NONE) {
        set_result(out, HEALTH_SKIP, "no reading");
        return;
    }
    int whole = tenths / 10, frac = tenths % 10;
    snprintf(out->text, sizeof(out->text), "chip %s%d.%d C", tenths < 0 && whole == 0 ? "-" : "",
             whole, frac < 0 ? -frac : frac);
    out->status = tenths > HEALTH_TEMP_FAIL ? HEALTH_FAIL
                : tenths > HEALTH_TEMP_WARN ? HEALTH_WARN : HEALTH_PASS;
}

void health_judge_supply(int vsys_min_mv, int usb, health_result_t *out) {
    if (vsys_min_mv == SENSORS_NONE) {
        set_result(out, HEALTH_SKIP, "no VSYS reading");
        return;
    }
    int cv = ((vsys_min_mv < 0 ? 0 : vsys_min_mv) + 5) / 10;      // hundredths of a volt
    snprintf(out->text, sizeof(out->text), "VSYS min %d.%02d V since the last check, %s",
             cv / 100, cv % 100,
             usb > 0 ? "USB power" : usb == 0 ? "no USB power" : "USB unknown");
    if (vsys_min_mv < HEALTH_VSYS_FAIL)
        out->status = HEALTH_FAIL;
    else if (usb > 0 && vsys_min_mv < HEALTH_VSYS_USB_WARN)
        out->status = HEALTH_WARN;
    else
        out->status = HEALTH_PASS;
}
