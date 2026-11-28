// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Phase 4's health checks, the reads (docs/design/health.md §9.5): `resets` with the reset history,
// `rtc-drift`, `heap-sum`, `core1`, and the brownout detector's register for `supply`. The judging
// is in bgcheck.c (pure, host tested). Each step takes microseconds; none holds a gate.

#include "core/bgcheck_board.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hardware/structs/powman.h"
#include "pico/stdlib.h"

#include "core/bgcheck.h"
#include "core/heap.h"
#include "core/heaptag.h"
#include "core/heapwalk.h"
#include "core/history.h"
#include "core/log.h"
#include "core/resetwhy.h"
#include "lua/lua_check.h"
#include "time/timesync.h"

// --- resets ------------------------------------------------------------------------------------------

#define LOOP_LOOK HISTORY_CARRY_BOOTS   // boot records looked at for a loop

static void resets_gather(bgcheck_resets_t *in) {
    const uint8_t *sector = history_sector();
    history_scan_t s;
    history_scan(sector, &s);
    static history_rec_t boots[LOOP_LOOK];      // off the stack: 16 x 20 bytes
    unsigned n = history_list(sector, HISTORY_KIND_BOOT, boots, LOOP_LOOK);
    *in = (bgcheck_resets_t){
        .why = resetwhy(),
        .text = resetwhy_text(),
        .history = s.state,
        .boots = s.boots,
        .damaged = s.damaged + s.stray,
        .short_runs = history_short_runs(boots, n, BGCHECK_STABLE_S),
    };
}

health_step_t bgcheck_resets_step(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (!cancel) {
        bgcheck_resets_t in;
        resets_gather(&in);
        bgcheck_judge_resets(&in, out);
    }
    return HEALTH_DONE;
}

// --- rtc-drift ---------------------------------------------------------------------------------------

health_step_t bgcheck_drift_step(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (!cancel) {
        timeconv_drift_t d;
        timesync_drift(&d);
        bgcheck_judge_drift(&d, out);
    }
    return HEALTH_DONE;
}

// --- heap-sum ----------------------------------------------------------------------------------------

static uintptr_t msplim(void) {
    uint32_t v;
    __asm volatile("mrs %0, msplim" : "=r"(v));
    return v;
}

static void heap_read(bgcheck_heap_read_t *r, uintptr_t *end) {
    heap_snapshot_t s;
    heap_snapshot(&s);
    size_t tags = 0;
    for (int t = 0; t < HEAP_TAGS; t++)
        tags += heap_tag_used((heap_tag_t)t);
    *r = (bgcheck_heap_read_t){s.used, s.free, s.arena, tags};
    *end = s.end;
}

static void heap_gather(bgcheck_heap_t *h) {
    extern char __end__, __HeapLimit, __StackLimit;
    memset(h, 0, sizeof(*h));
    heap_read(&h->a, &h->end);
    h->b = h->a;
    if (h->a.tags > h->a.used) {
        // The tags are counted outside the allocator's lock: a shrinking realloc counts more
        // than the heap for a moment. Only a second reading decides.
        busy_wait_us(1000);
        heap_read(&h->b, &h->end);
    }
    h->start = (uintptr_t)&__end__;
    uintptr_t hl = (uintptr_t)&__HeapLimit, sl = (uintptr_t)&__StackLimit;
    h->limit = hl < sl ? hl : sl;
    h->msplim = msplim();
}

health_step_t bgcheck_heap_step(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (!cancel) {
        bgcheck_heap_t h;
        heap_gather(&h);
        bgcheck_judge_heap(&h, out);
    }
    return HEALTH_DONE;
}

// --- supply ------------------------------------------------------------------------------------------

uint32_t bgcheck_bod_register(void) {
    return powman_hw->bod;
}

#ifdef SOS_TEST_COMMANDS
#define DUMP_LINE 256

__attribute__((format(printf, 3, 4)))
static void dump(health_print_fn print, void *arg, const char *fmt, ...) {
    char line[DUMP_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    print(arg, line);
}

// --- health heap-walk (test builds) ------------------------------------------------------------------
//
// newlib's internals, in test builds only (docs/design/health.md §9.5): the bin array and the
// arena's start, which mallocr.c exports. Built only for the layout heapwalk.h describes.

#include <malloc.h>
#include <newlib.h>
#include <reent.h>
#include <stdlib.h>
#include <sys/config.h>
#include <unistd.h>

#if defined(_NEWLIB_VERSION) && __NEWLIB__ == 4 && \
    (!defined(MALLOC_ALIGNMENT) || MALLOC_ALIGNMENT == 8)
#define HEAPWALK_BUILT 1
#if __NEWLIB_MINOR__ != 6
#warning "health heap-walk was checked against newlib 4.6 only: check mallocr.c's chunk layout"
#endif
_Static_assert(sizeof(size_t) == 4 && sizeof(void *) == 4, "heapwalk.h's layout is 32-bit");
extern void *__malloc_av_[];
extern char *__malloc_sbrk_base;
#endif

#define WALK_BUDGET_US 8000
#define KNOWN_REQUEST  100              // the known block's size

static uint64_t walk_t0;

static uint32_t walk_read(void *ctx, uint32_t addr) {
    (void)ctx;
    return *(const volatile uint32_t *)(uintptr_t)addr;
}

static bool walk_over(void *ctx) {
    (void)ctx;
    return time_us_64() - walk_t0 > WALK_BUDGET_US;
}

void bgcheck_board_heap_walk(health_print_fn print, void *arg) {
#ifndef HEAPWALK_BUILT
    print(arg, "heap-walk: not built for this C library (newlib 4, 8-byte alignment, only)");
#else
    // The known block is allocated before the lock and freed after it: pico_malloc takes its
    // own mutex before newlib's, so a malloc while holding newlib's could deadlock with core 1.
    void *known = malloc(KNOWN_REQUEST);
    if (!known) {
        print(arg, "heap-walk: no memory for the known block");
        return;
    }
    heapwalk_result_t r;
    __malloc_lock(_REENT);
    uintptr_t base = (uintptr_t)__malloc_sbrk_base;
    heapwalk_view_t v = {
        .read = walk_read,
        .over = walk_over,
        .first = (uint32_t)((base + 7) & ~(uintptr_t)7),
        .top = (uint32_t)(uintptr_t)__malloc_av_[2],
        .end = (uint32_t)(uintptr_t)sbrk(0),
        .av = (uint32_t)(uintptr_t)__malloc_av_,
        .known = (uint32_t)((uintptr_t)known - 8),
    };
    walk_t0 = time_us_64();
    heapwalk_run(&v, heapwalk_chunk_size(KNOWN_REQUEST), &r);
    unsigned us = (unsigned)(time_us_64() - walk_t0);
    __malloc_unlock(_REENT);
    free(known);

    dump(print, arg, "heap-walk: newlib %s; arena 0x%08lx to 0x%08lx, top 0x%08lx; %u us",
         _NEWLIB_VERSION, (unsigned long)v.first, (unsigned long)v.end, (unsigned long)v.top, us);
    dump(print, arg, "chunks %lu (in use %lu KB), free %lu (%lu KB, largest %lu KB); free lists "
         "%lu nodes, %lu KB", (unsigned long)r.chunks, (unsigned long)(r.used_bytes / 1024),
         (unsigned long)r.free_chunks, (unsigned long)(r.free_bytes / 1024),
         (unsigned long)(r.largest_free / 1024), (unsigned long)r.bin_nodes,
         (unsigned long)(r.bin_bytes / 1024));
    if (r.error == HEAPWALK_OK) {
        dump(print, arg, "known block found in use (%lu bytes); %s", (unsigned long)r.known_size,
             heapwalk_error_text(r.error));
        return;
    }
    char words[48] = "";
    if (r.at >= v.first && r.at + 16 <= v.end)
        snprintf(words, sizeof(words), "; words %08lx %08lx %08lx %08lx",
                 (unsigned long)walk_read(NULL, r.at), (unsigned long)walk_read(NULL, r.at + 4),
                 (unsigned long)walk_read(NULL, r.at + 8), (unsigned long)walk_read(NULL, r.at + 12));
    dump(print, arg, "%s: %s at 0x%08lx (bin %u)%s", r.error == HEAPWALK_STOPPED ? "STOPPED"
         : "BROKEN", heapwalk_error_text(r.error), (unsigned long)r.at, r.bin, words);
    if (r.error != HEAPWALK_STOPPED)
        log_msg(SOS_LOG_KERN | SOS_LOG_ERR, "heap-walk: %s at 0x%08lx (bin %u)%s",
                heapwalk_error_text(r.error), (unsigned long)r.at, r.bin, words);
#endif
}

// --- health state-dump's phase 4 lines -----------------------------------------------------------------

static void dump_history(health_print_fn print, void *arg) {
    const uint8_t *sector = history_sector();
    history_scan_t s;
    history_scan(sector, &s);
    static const char *const state[] = {"absent (image overlaps)", "erased", "ours", "newer format",
                                        "foreign"};
    dump(print, arg, "history: %s, %u used of %u, %u valid (%u boots), %u damaged, %u stray",
         state[s.state], s.next, (unsigned)HISTORY_SLOTS, s.valid, s.boots, s.damaged, s.stray);
    static history_rec_t recs[8];
    unsigned n = history_list(sector, 0, recs, 8);
    for (unsigned i = 0; i < n; i++) {
        const history_rec_t *r = &recs[i];
        if (r->kind == HISTORY_KIND_RTC)
            dump(print, arg, "history: rtc set to %lu, cal steps %d, %s", (unsigned long)r->wall,
                 r->cal_steps, r->confirmed ? "confirmed" : "unconfirmed");
        else if (r->kind == HISTORY_KIND_BOOT)
        {
            // "?" for what isn't known, as `uptime history` shows it (a brownout, power-on or
            // reset-pin boot keeps no uptime; a boot before the clock was set has no time).
            char up[16], at[16];
            if (r->uptime_s == HISTORY_NONE)
                snprintf(up, sizeof(up), "?");
            else
                snprintf(up, sizeof(up), "%lu", (unsigned long)r->uptime_s);
            if (r->wall == HISTORY_NONE)
                snprintf(at, sizeof(at), "?");
            else
                snprintf(at, sizeof(at), "%lu", (unsigned long)r->wall);
            dump(print, arg, "history: boot %u, %s, up %s s before, at %s, extra 0x%02x",
                 r->boot_no, resetwhy_name((resetwhy_t)r->reason), up, at, r->extra);
        }
        else
            dump(print, arg, "history: test record");
    }
}

void bgcheck_board_dump(health_print_fn print, void *arg) {
    dump(print, arg, "reset: CHIP_RESET 0x%08lx, watchdog REASON 0x%lx: %s",
         (unsigned long)resetwhy_chip_reset(), (unsigned long)resetwhy_wd_reason(),
         resetwhy_text());
    dump(print, arg, "brownout detector: BOD 0x%08lx (default 0x%03x: on at 0.946 V)",
         (unsigned long)bgcheck_bod_register(), BGCHECK_BOD_DEFAULT);
    dump_history(print, arg);

    timeconv_drift_t d;
    timesync_drift(&d);
    dump(print, arg, "drift: rtc present %d, state %d, err %lld ms +- %lu over %lu s, cal steps %d; "
         "xosc %d, err %lld ms +- %lu over %lu s", d.rtc_present, d.rtc, (long long)d.rtc_err_ms,
         (unsigned long)d.rtc_unc_ms, (unsigned long)d.rtc_interval_s, d.cal_steps, d.xosc,
         (long long)d.xosc_err_ms, (unsigned long)d.xosc_unc_ms, (unsigned long)d.xosc_interval_s);

    bgcheck_heap_t h;
    heap_gather(&h);
    dump(print, arg, "heap: used %u, free %u, arena %u, tags %u; start 0x%08lx, end 0x%08lx, "
         "limit 0x%08lx, msplim 0x%08lx", (unsigned)h.a.used, (unsigned)h.a.free,
         (unsigned)h.a.arena, (unsigned)h.a.tags, (unsigned long)h.start, (unsigned long)h.end,
         (unsigned long)h.limit, (unsigned long)h.msplim);

    lua_check_dump(print, arg);
}
#endif
