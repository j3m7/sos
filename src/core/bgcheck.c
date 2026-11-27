// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See bgcheck.h.

#include "core/bgcheck.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

__attribute__((format(printf, 3, 4)))
static void set(health_result_t *out, health_status_t status, const char *fmt, ...) {
    out->status = status;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out->text, sizeof(out->text), fmt, ap);
    va_end(ap);
}

// --- resets ------------------------------------------------------------------------------------------

// "last reset: ..." for a reason worth a WARN. A fault's record names the fault and where
// ("hard fault on core 0 at 0x1000409c"); the rest (caller, CFSR) wouldn't fit and would be cut
// mid-number, so only its first part, and `uptime` shows all of it.
static void last_reset(const bgcheck_resets_t *in, health_result_t *out) {
    const char *t = in->text ? in->text : resetwhy_name(in->why);
    switch (in->why) {
    case RESETWHY_FAULT: {
        int head = (int)strcspn(t, ",");
        set(out, HEALTH_WARN, "last reset: %.*s%s", head, t, t[head] ? " (see uptime)" : "");
        break;
    }
    case RESETWHY_BROWNOUT:
        set(out, HEALTH_WARN, "last reset: brownout (core supply under 0.95 V)");
        break;
    default:
        set(out, HEALTH_WARN, "last reset: %s", t);
        break;
    }
}

void bgcheck_judge_resets(const bgcheck_resets_t *in, health_result_t *out) {
    const char *name = resetwhy_name(in->why);
    if (in->short_runs >= BGCHECK_LOOP_RESETS) {
        set(out, HEALTH_WARN, "%u short runs in a row; last reset: %s", in->short_runs, name);
    } else if (resetwhy_warns(in->why)) {
        last_reset(in, out);
    } else if (in->history == HISTORY_OURS && in->damaged) {
        set(out, HEALTH_WARN, "history: %u damaged record%s skipped; last reset: %s",
            in->damaged, in->damaged == 1 ? "" : "s", name);
    } else if (in->history == HISTORY_ABSENT) {
        set(out, HEALTH_WARN, "%s; history off: the image overlaps its sector", name);
    } else if (in->history == HISTORY_NEWER) {
        set(out, HEALTH_WARN, "%s; history sector of a newer format: not written", name);
    } else if (!in->boots) {
        set(out, HEALTH_PASS, "%s; no boots recorded", name);
    } else {
        set(out, HEALTH_PASS, "%s; %u boot%s recorded, no loop", name, in->boots,
            in->boots == 1 ? "" : "s");
    }
}

// --- rtc-drift ---------------------------------------------------------------------------------------

// A part's text is built longer than a result can hold, and cut when it is copied into one (the
// compiler can't know the pieces are short).
typedef struct {
    health_status_t status;
    char text[128];
} part_t;

static void put_text(health_result_t *out, const char *src) {
    size_t n = strlen(src);
    if (n > sizeof(out->text) - 1)
        n = sizeof(out->text) - 1;
    memcpy(out->text, src, n);
    out->text[n] = '\0';
}

static void rtc_part(const timeconv_drift_t *d, part_t *p) {
    char cal[16], rate[24], span[12];
    timeconv_format_cal(cal, sizeof(cal), d->cal_steps);
    switch (d->rtc) {
    case TIMECONV_DRIFT_NO_BASELINE:
        p->status = HEALTH_SKIP;
        snprintf(p->text, sizeof(p->text), "RTC: no baseline; drift after two syncs (%s)", cal);
        return;
    case TIMECONV_DRIFT_BASELINE:
        p->status = HEALTH_SKIP;
        if (d->rtc_interval_s) {
            timeconv_format_span(span, sizeof(span), d->rtc_interval_s);
            snprintf(p->text, sizeof(p->text), "RTC set %s ago (%s): drift at the next sync",
                     span, cal);
        } else {
            snprintf(p->text, sizeof(p->text), "RTC set at a sync (%s): drift at the next", cal);
        }
        return;
    case TIMECONV_DRIFT_CAL_CHANGED:
        p->status = HEALTH_SKIP;
        snprintf(p->text, sizeof(p->text), "calibration changed since the RTC was set; next sync");
        return;
    case TIMECONV_DRIFT_MEASURED:
        break;
    }
    int64_t r = timeconv_rate_mppm(d->rtc_err_ms, d->rtc_interval_s);
    int64_t u = timeconv_rate_mppm(d->rtc_unc_ms, d->rtc_interval_s);
    timeconv_format_rate(rate, sizeof(rate), r, u);
    timeconv_format_span(span, sizeof(span), d->rtc_interval_s);
    int v = timeconv_verdict(r, u, BGCHECK_RTC_WARN_MPPM);
    if (v < 0) {
        p->status = HEALTH_PASS;
        snprintf(p->text, sizeof(p->text), "RTC %s in %s, %s", rate, span, cal);
    } else if (v == 0) {
        p->status = HEALTH_SKIP;
        snprintf(p->text, sizeof(p->text), "RTC %s in %s, %s: too short to judge", rate, span,
                 cal);
    } else {
        // The calibration that would correct it: the one in force plus the rate measured.
        int64_t want = timeconv_cal_mppm(d->cal_steps) + r;
        p->status = HEALTH_WARN;
        if (want < BGCHECK_CAL_MIN_MPPM || want > BGCHECK_CAL_MAX_MPPM) {
            snprintf(p->text, sizeof(p->text), "RTC %s in %s: beyond time cal's range", rate,
                     span);
        } else {
            char ppm[16];
            timeconv_format_ppm(ppm, sizeof(ppm), want);
            snprintf(p->text, sizeof(p->text), "RTC %s in %s, %s: try time cal %s", rate, span,
                     cal, ppm);
        }
    }
}

static void xosc_part(const timeconv_drift_t *d, part_t *p) {
    char rate[24], span[12];
    if (!d->xosc) {
        p->status = HEALTH_NONE;
        p->text[0] = '\0';
        return;
    }
    int64_t r = timeconv_rate_mppm(d->xosc_err_ms, d->xosc_interval_s);
    int64_t u = timeconv_rate_mppm(d->xosc_unc_ms, d->xosc_interval_s);
    timeconv_format_rate(rate, sizeof(rate), r, u);
    timeconv_format_span(span, sizeof(span), d->xosc_interval_s);
    if (timeconv_verdict(r, u, BGCHECK_XOSC_FAIL_MPPM) > 0) {
        p->status = HEALTH_FAIL;
        snprintf(p->text, sizeof(p->text), "XOSC %s: beyond the chip's 1000 ppm margin", rate);
    } else if (timeconv_verdict(r, u, BGCHECK_XOSC_WARN_MPPM) > 0) {
        p->status = HEALTH_WARN;
        snprintf(p->text, sizeof(p->text), "XOSC %s in %s (crystal: 30+30 ppm)", rate, span);
    } else if (timeconv_verdict(r, u, BGCHECK_XOSC_WARN_MPPM) < 0) {
        p->status = HEALTH_PASS;
        snprintf(p->text, sizeof(p->text), "XOSC %s in %s", rate, span);
    } else {
        p->status = HEALTH_SKIP;
        snprintf(p->text, sizeof(p->text), "XOSC %s in %s: too short to judge", rate, span);
    }
}

// One part, then the other after "; " if the whole fits in a result (under 63 characters: a text
// of 63 may have been cut, and the tests refuse it).
static void join(health_result_t *out, const char *first, const char *second) {
    char both[2 * sizeof(((part_t *)0)->text) + 2];
    if (second[0] && strlen(first) + 2 + strlen(second) < sizeof(out->text) - 1)
        snprintf(both, sizeof(both), "%s; %s", first, second);
    else
        snprintf(both, sizeof(both), "%s", first);
    put_text(out, both);
}

void bgcheck_judge_drift(const timeconv_drift_t *d, health_result_t *out) {
    part_t x;
    xosc_part(d, &x);
    if (!d->rtc_present) {
        if (x.status == HEALTH_NONE) {
            set(out, HEALTH_SKIP, "no RTC; XOSC not measured (two syncs in one uptime)");
        } else {
            out->status = x.status;
            join(out, "no RTC", x.text);
        }
        return;
    }
    part_t r;
    rtc_part(d, &r);
    if (x.status > r.status) {
        out->status = x.status;
        join(out, x.text, r.text);
    } else {
        out->status = r.status;
        join(out, r.text, x.text);
    }
}

// --- heap-sum ------------------------------------------------------------------------------------------

static unsigned kb(size_t n) {
    return (unsigned)((n + 512) / 1024);
}

void bgcheck_judge_heap(const bgcheck_heap_t *h, health_result_t *out) {
    const bgcheck_heap_read_t *a = &h->a, *b = &h->b;
    if (h->end > h->limit) {
        set(out, HEALTH_FAIL, "heap end 0x%08lx beyond its limit 0x%08lx", (unsigned long)h->end,
            (unsigned long)h->limit);
    } else if (h->msplim && h->end > h->msplim) {
        set(out, HEALTH_FAIL, "heap end 0x%08lx beyond the stack guard 0x%08lx",
            (unsigned long)h->end, (unsigned long)h->msplim);
    } else if (a->tags > a->used && b->tags > b->used) {
        set(out, HEALTH_WARN, "tags hold %u KB, heap in use %u KB: a double release?",
            kb(b->tags), kb(b->used));
    } else if (a->used + a->free != a->arena) {
        set(out, HEALTH_WARN, "mallinfo: in use + free %u KB, arena %u KB", kb(a->used + a->free),
            kb(a->arena));
    } else if (h->end < h->start + a->arena) {
        set(out, HEALTH_WARN, "heap ends %u KB in, arena %u KB", kb(h->end - h->start),
            kb(a->arena));
    } else {
        set(out, HEALTH_PASS, "in use %u KB (tags %u KB), arena %u KB of %u KB", kb(a->used),
            kb(a->tags), kb(a->arena), kb(h->limit - h->start));
    }
}

// --- core1 -------------------------------------------------------------------------------------------

void bgcheck_judge_core1(bgcheck_core1_state_t *st, const bgcheck_core1_in_t *in,
                         health_result_t *out) {
    if (!in->running) {
        if (st->troubled)
            set(out, HEALTH_PASS, "core 1 free again: script ended after %lu s",
                (unsigned long)st->run_s);
        else
            set(out, HEALTH_SKIP, "no script running");
        memset(st, 0, sizeof(*st));
        return;
    }
    const char *name = in->name ? in->name : "script";
    if (!st->tracking) {
        memset(st, 0, sizeof(*st));
        st->tracking = true;
        st->beats = in->beats;
        st->at_us = st->moved_us = in->now_us;
    }
    uint32_t delta = in->beats - st->beats;
    uint64_t dt = in->now_us - st->at_us;
    if (delta || in->waiting != BGCHECK_WAIT_NONE)
        st->moved_us = in->now_us;
    if (!in->cancel)
        st->stop_us = 0;
    else if (!st->stop_us)
        st->stop_us = in->now_us;
    st->beats = in->beats;
    st->at_us = in->now_us;
    st->run_s = in->run_s;
    uint64_t still = in->now_us - st->moved_us;

    if (in->cancel && in->now_us - st->stop_us >= BGCHECK_CORE1_STOP_US &&
        still >= BGCHECK_CORE1_STOP_US) {
        st->troubled = true;
        set(out, HEALTH_FAIL, "core 1 ignored stop for %lu s: no scripts until reboot",
            (unsigned long)((in->now_us - st->stop_us) / 1000000));
    } else if (!in->cancel && still >= BGCHECK_CORE1_WARN_US) {
        st->troubled = true;
        set(out, HEALTH_WARN, "%.16s in one C call for %lu s: Ctrl-C waits for it", name,
            (unsigned long)(still / 1000000));
    } else if (in->waiting != BGCHECK_WAIT_NONE) {
        static const char *const what[] = {"", "core 0", "room for output", "a line"};
        set(out, HEALTH_PASS, "%.16s waiting for %s, running %lu s", name,
            what[in->waiting <= BGCHECK_WAIT_LINE ? in->waiting : 0], (unsigned long)in->run_s);
    } else if (dt && delta) {
        set(out, HEALTH_PASS, "%.16s running %lu s, %lu hooks/s", name, (unsigned long)in->run_s,
            (unsigned long)((uint64_t)delta * 1000000 / dt));
    } else {
        set(out, HEALTH_PASS, "%.16s running %lu s", name, (unsigned long)in->run_s);
    }
}

// --- supply's brownout detector ------------------------------------------------------------------------

void bgcheck_judge_bod(uint32_t bod, health_result_t *out) {
    if ((bod & BGCHECK_BOD_MASK) != BGCHECK_BOD_DEFAULT) {
        if (out->status < HEALTH_WARN)
            set(out, HEALTH_WARN, "brownout detector off or moved (BOD 0x%08lx)",
                (unsigned long)bod);
        return;
    }
    if (out->status == HEALTH_PASS && strlen(out->text) + 8 < sizeof(out->text))
        strcat(out->text, "; BOD on");
}
