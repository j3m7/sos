// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See countcheck.h. Texts stay under HEALTH_TEXT_MAX - 1 characters (tests/test_countcheck.c
// checks every one it makes).

#include "core/countcheck.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/countcheck_names.h"

static void set(health_result_t *out, health_status_t status, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void set(health_result_t *out, health_status_t status, const char *fmt, ...) {
    out->status = status;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out->text, sizeof(out->text), fmt, ap);
    va_end(ap);
}

// Adds "; <tail>" to the text if the whole still fits on a line (under HEALTH_TEXT_MAX - 1).
static void append_if_fits(health_result_t *out, const char *tail) {
    size_t len = strlen(out->text);
    if (len + 2 + strlen(tail) < sizeof(out->text) - 1)
        snprintf(out->text + len, sizeof(out->text) - len, "; %s", tail);
}

// "42 min", "3 h", "2 d": how long, roughly. Under two minutes, seconds.
static void duration(char *buf, size_t cap, uint32_t s) {
    if (s < 120)
        snprintf(buf, cap, "%lu s", (unsigned long)s);
    else if (s < 120 * 60)
        snprintf(buf, cap, "%lu min", (unsigned long)(s / 60));
    else if (s < 48 * 3600)
        snprintf(buf, cap, "%lu h", (unsigned long)(s / 3600));
    else
        snprintf(buf, cap, "%lu d", (unsigned long)(s / 86400));
}

// A window's length as the texts give it: "1 h" once it is an hour, else "42 min".
static void window_text(char *buf, size_t cap, uint32_t span_s) {
    if (span_s >= COUNTCHECK_WINDOW_S)
        snprintf(buf, cap, "1 h");
    else
        snprintf(buf, cap, "%lu min", (unsigned long)(span_s / 60));
}

// --- windows -------------------------------------------------------------------------------------

void countcheck_window(countcheck_window_t *w, uint32_t now_s,
                       const uint32_t now_v[COUNTCHECK_VALUES],
                       uint32_t delta[COUNTCHECK_VALUES], uint32_t *span_s) {
    // The newest sample at least an hour old; none: boot, when every counter was 0.
    const countcheck_sample_t *base = NULL;
    for (unsigned i = 0; i < w->n; i++) {
        const countcheck_sample_t *s = &w->s[i];
        if (now_s - s->at_s >= COUNTCHECK_WINDOW_S && s->at_s <= now_s &&
            (!base || s->at_s > base->at_s))
            base = s;
    }
    for (unsigned i = 0; i < COUNTCHECK_VALUES; i++)
        delta[i] = now_v[i] - (base ? base->v[i] : 0);     // counters only grow (and wrap)
    *span_s = now_s - (base ? base->at_s : 0);

    const countcheck_sample_t *newest = w->n ? &w->s[(w->next + COUNTCHECK_SAMPLES - 1) %
                                                     COUNTCHECK_SAMPLES] : NULL;
    if (newest && now_s - newest->at_s < COUNTCHECK_GAP_S)
        return;
    countcheck_sample_t *s = &w->s[w->next];
    s->at_s = now_s;
    memcpy(s->v, now_v, sizeof(s->v));
    w->next = (w->next + 1) % COUNTCHECK_SAMPLES;
    if (w->n < COUNTCHECK_SAMPLES)
        w->n++;
}

// --- wifi ----------------------------------------------------------------------------------------

unsigned countcheck_drops_within(const uint32_t *times, unsigned n, uint32_t now_s,
                                 uint32_t within_s, uint32_t *newest_ago_s) {
    unsigned count = 0;
    uint32_t newest = UINT32_MAX;
    for (unsigned i = 0; i < n; i++) {
        if (times[i] > now_s)
            continue;
        uint32_t ago = now_s - times[i];
        if (ago < within_s) {
            count++;
            if (ago < newest)
                newest = ago;
        }
    }
    *newest_ago_s = count ? newest : 0;
    return count;
}

bool countcheck_fw_version(const char *ver, char *out, unsigned cap) {
    if (cap)
        out[0] = '\0';
    const char *p = ver ? strstr(ver, "version ") : NULL;
    if (!p || !cap)
        return false;
    p += strlen("version ");
    if (*p < '0' || *p > '9')
        return false;
    unsigned n = 0;
    while (n + 1 < cap && ((*p >= '0' && *p <= '9') || *p == '.'))
        out[n++] = *p++;
    out[n] = '\0';
    return true;
}

countcheck_wifi_kind_t countcheck_wifi_warn_kind(const char *func) {
#define WIFI_ENTRY(name, kind) if (func && !strcmp(func, name)) return kind;
    COUNTCHECK_WIFI_FUNCS(WIFI_ENTRY)
#undef WIFI_ENTRY
    return COUNTCHECK_WIFI_OTHER;
}

static const char *plural(unsigned long n, const char *one, const char *many) {
    return n == 1 ? one : many;
}

void countcheck_judge_wifi(const countcheck_wifi_t *in, health_result_t *out) {
    if (in->state == COUNTCHECK_WIFI_UNSET) {
        set(out, HEALTH_SKIP, "Wi-Fi not set up");
        return;
    }
    if (in->state == COUNTCHECK_WIFI_STOPPED) {
        set(out, HEALTH_SKIP, "Wi-Fi stopped");
        return;
    }
    char win[16], fw[32], ago[16];
    window_text(win, sizeof(win), in->span_s);
    bool have_fw = in->fw && in->fw[0];
    snprintf(fw, sizeof(fw), "fw %s", have_fw ? in->fw : "?");
    unsigned long errors = (unsigned long)in->timeouts + in->io + in->bus;

    if (errors >= COUNTCHECK_WIFI_FAIL_ERRORS) {
        set(out, HEALTH_FAIL, "%lu driver errors in %s: the chip isn't answering", errors, win);
        return;
    }
    if (errors >= COUNTCHECK_WIFI_WARN_ERRORS) {
        char parts[40] = "";
        size_t len = 0;
        if (in->timeouts)
            len += (size_t)snprintf(parts + len, sizeof(parts) - len, "%lu %s",
                                    (unsigned long)in->timeouts,
                                    plural(in->timeouts, "timeout", "timeouts"));
        if (in->io && len < sizeof(parts))
            len += (size_t)snprintf(parts + len, sizeof(parts) - len, "%s%lu I/O",
                                    len ? ", " : "", (unsigned long)in->io);
        if (in->bus && len < sizeof(parts))
            snprintf(parts + len, sizeof(parts) - len, "%s%lu bus", len ? ", " : "",
                     (unsigned long)in->bus);
        set(out, HEALTH_WARN, "%lu driver errors in %s (%s)", errors, win, parts);
        append_if_fits(out, fw);
        return;
    }
    if (in->drops >= COUNTCHECK_WIFI_WARN_DROPS) {
        duration(ago, sizeof(ago), in->last_drop_ago_s);
        set(out, HEALTH_WARN, "%u link drops in 24 h, the last %s ago", in->drops, ago);
        append_if_fits(out, fw);
        return;
    }
    if (in->state == COUNTCHECK_WIFI_DOWN && in->down_s >= COUNTCHECK_WIFI_DOWN_S) {
        duration(ago, sizeof(ago), in->down_s);
        set(out, HEALTH_WARN, "no connection for %s: check the network and 'wifi'", ago);
        return;
    }
    if (!have_fw) {
        set(out, HEALTH_WARN, "the chip didn't give its firmware version");
        return;
    }

    char errs[40], drops[32];
    if (errors)
        snprintf(errs, sizeof(errs), "%lu %s in %s", errors, plural(errors, "error", "errors"),
                 win);
    else
        snprintf(errs, sizeof(errs), "no errors in %s", win);
    if (in->drops)
        snprintf(drops, sizeof(drops), "%u %s in 24 h", in->drops,
                 plural(in->drops, "drop", "drops"));
    else
        snprintf(drops, sizeof(drops), "no drops in 24 h");
    if (in->state == COUNTCHECK_WIFI_DOWN) {
        duration(ago, sizeof(ago), in->down_s);
        set(out, HEALTH_PASS, "no connection for %s; %s; %s", ago, fw, errs);
        return;
    }
    set(out, HEALTH_PASS, "%s, driver %s; %s; %s", fw, in->driver ? in->driver : "?", errs,
        drops);
    if (strlen(out->text) >= sizeof(out->text) - 1)
        set(out, HEALTH_PASS, "%s; %s; %s", fw, errs, drops);
}

// --- sd-bus --------------------------------------------------------------------------------------

countcheck_sd_kind_t countcheck_sd_kind(const char *func, bool initialising, bool error_line) {
#define SD_ENTRY(name, kind, during_init, from_error_lines)                                      \
    if (func && !strcmp(func, name))                                                            \
        return (initialising && !(during_init)) || error_line != (from_error_lines)             \
               ? COUNTCHECK_SD_NONE : (kind);
    COUNTCHECK_SD_FUNCS(SD_ENTRY)
#undef SD_ENTRY
    return COUNTCHECK_SD_NONE;
}

// "25.0 MHz"
static void mhz(char *buf, size_t cap, uint32_t khz) {
    snprintf(buf, cap, "%lu.%lu MHz", (unsigned long)(khz / 1000),
             (unsigned long)(khz % 1000 / 100));
}

// "512 KB", "37 MB", "12 GB"
static void amount(char *buf, size_t cap, uint64_t bytes) {
    if (bytes < 1024 * 1024)
        snprintf(buf, cap, "%lu KB", (unsigned long)(bytes / 1024));
    else if (bytes < 10ull * 1024 * 1024 * 1024)
        snprintf(buf, cap, "%lu MB", (unsigned long)(bytes / (1024 * 1024)));
    else
        snprintf(buf, cap, "%lu GB", (unsigned long)(bytes / (1024ull * 1024 * 1024)));
}

void countcheck_judge_sd(const countcheck_sd_t *in, health_result_t *out) {
    if (!in->ever_ok) {
        set(out, HEALTH_SKIP, "no card initialised since boot");
        return;
    }
    char clk[16], moved[16];
    mhz(clk, sizeof(clk), in->khz);
    amount(moved, sizeof(moved), in->bytes);
    unsigned long failed = (unsigned long)in->failed_reads + in->failed_writes;
    unsigned long retried = (unsigned long)in->crc + in->cmd + in->write;
    bool fast = in->khz > COUNTCHECK_SD_SPEC_KHZ;

    if (failed >= COUNTCHECK_SD_FAIL_FAILED) {
        set(out, HEALTH_FAIL, "%lu failed reads/writes since init: card or bus failing", failed);
        return;
    }
    if (failed) {
        char parts[48] = "";
        size_t len = 0;
        if (in->failed_writes)
            len += (size_t)snprintf(parts, sizeof(parts), "%lu failed %s",
                                    (unsigned long)in->failed_writes,
                                    plural(in->failed_writes, "write", "writes"));
        if (in->failed_reads && len < sizeof(parts))
            snprintf(parts + len, sizeof(parts) - len, "%s%lu failed %s", len ? ", " : "",
                     (unsigned long)in->failed_reads, plural(in->failed_reads, "read", "reads"));
        set(out, HEALTH_WARN, "%s since init: sd check", parts);
        return;
    }
    if (in->last_init_failed) {
        set(out, HEALTH_WARN, "card no longer initialises at %s (removed?)", clk);
        return;
    }
    if (in->timeouts) {
        set(out, HEALTH_WARN, "card timed out %lux since init (2 s each); %s",
            (unsigned long)in->timeouts, clk);
        return;
    }
    if (fast && retried) {
        set(out, HEALTH_WARN, "%lu retried at %s, over SD's 25 MHz: sd speed 25", retried, clk);
        return;
    }
    if (retried >= COUNTCHECK_SD_RETRY_MIN &&
        (uint64_t)retried * COUNTCHECK_SD_RETRY_BYTES >= in->bytes) {
        set(out, HEALTH_WARN, "%s; %lu retried in %s: over 1 per 10 MB", clk, retried, moved);
        return;
    }
    if (retried)
        set(out, HEALTH_PASS, "%s%s; %lu retried in %s (under 1 per 10 MB)", fast ? "at " : "",
            clk, retried, moved);
    else
        set(out, HEALTH_PASS, "%s%s; %s since the card's init, no errors", fast ? "at " : "",
            clk, moved);
}

// --- net-pools -----------------------------------------------------------------------------------

void countcheck_judge_pools(const countcheck_pools_t *in, health_result_t *out) {
    char win[16];
    window_text(win, sizeof(win), in->span_s);
    if (in->pbuf)
        set(out, HEALTH_WARN, "packet pool empty %lux in %s: received frames dropped",
            (unsigned long)in->pbuf, win);
    else if (in->seg)
        set(out, HEALTH_WARN, "segment pool full %lux in %s: sends refused",
            (unsigned long)in->seg, win);
    else if (in->heap)
        set(out, HEALTH_WARN, "lwIP heap full %lux in %s: sends refused",
            (unsigned long)in->heap, win);
    else
        set(out, HEALTH_PASS, "no failures in %s; packets most %u of %u", win, in->pool_max,
            in->pool_avail);
}
