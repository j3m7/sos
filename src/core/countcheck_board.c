// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Phase 6's health checks, the reads (docs/design/health.md §9.9): `wifi`, `sd-bus` and
// `net-pools`. Each step reads counters others keep in RAM (net/wifistat.c, net/wifi.c,
// files/sdcard.c, lwIP's) and hands them to countcheck.c to judge: microseconds, no ioctl to the
// Wi-Fi chip, nothing sent to the card. None holds a gate.

#include "core/countcheck_board.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "lwip/stats.h"

#include "core/countcheck.h"
#include "files/sdcard.h"
#include "net/wifi.h"
#include "net/wifistat.h"

static uint32_t uptime_s(void) {
    return (uint32_t)(time_us_64() / 1000000);
}

// --- wifi ----------------------------------------------------------------------------------------

static countcheck_window_t wifi_window;  // timeouts, I/O, bus

typedef struct {
    countcheck_wifi_t in;
    wifistat_t ws;
    wifi_counts_t wc;
    char fw[16];
    char driver[16];
} wifi_read_t;

// What `wifi` judges, the window taken over `win` (the check's own, or a copy for the dump).
static void wifi_gather(wifi_read_t *r, countcheck_window_t *win) {
    uint32_t now = uptime_s();
    wifi_counts(&r->wc);
    wifistat_get(&r->ws);
    countcheck_fw_version(r->wc.ver, r->fw, sizeof(r->fw));
    snprintf(r->driver, sizeof(r->driver), "%d.%d.%d", CYW43_VERSION_MAJOR, CYW43_VERSION_MINOR,
             CYW43_VERSION_MICRO);
    uint32_t v[COUNTCHECK_VALUES] = {r->ws.timeout, r->ws.io, r->ws.bus}, d[COUNTCHECK_VALUES];
    uint32_t span;
    countcheck_window(win, now, v, d, &span);
    countcheck_wifi_t *in = &r->in;
    *in = (countcheck_wifi_t){
        .state = !r->wc.configured ? COUNTCHECK_WIFI_UNSET
               : !r->wc.running ? COUNTCHECK_WIFI_STOPPED
               : r->wc.up ? COUNTCHECK_WIFI_UP : COUNTCHECK_WIFI_DOWN,
        .down_s = now - r->wc.down_since_s,
        .timeouts = d[0], .io = d[1], .bus = d[2],
        .span_s = span,
        .fw = r->fw[0] ? r->fw : NULL,
        .driver = r->driver,
    };
    in->drops = countcheck_drops_within(r->wc.drop_at_s, r->wc.drop_n, now,
                                        COUNTCHECK_DROP_WINDOW_S, &in->last_drop_ago_s);
}

health_step_t countcheck_wifi_step(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (!cancel) {
        static wifi_read_t r;           // off the stack: the counts and a 128-byte line pointer
        wifi_gather(&r, &wifi_window);
        countcheck_judge_wifi(&r.in, out);
    }
    return HEALTH_DONE;
}

// --- sd-bus --------------------------------------------------------------------------------------

static void sd_gather(countcheck_sd_t *in, sdcard_counters_t *c) {
    sdcard_counters(c);
    const sdcard_ctr_t *i = &c->since_init;
    *in = (countcheck_sd_t){
        .ever_ok = c->ever_ok,
        .last_init_failed = c->last_init_failed,
        .khz = c->khz,
        .bytes = (i->sectors_read + i->sectors_written) * 512,
        .failed_reads = i->failed_reads,
        .failed_writes = i->failed_writes,
        .crc = i->crc,
        .cmd = i->cmd,
        .write = i->write,
        .timeouts = i->timeout,
    };
}

health_step_t countcheck_sd_step(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (!cancel) {
        countcheck_sd_t in;
        sdcard_counters_t c;
        sd_gather(&in, &c);
        countcheck_judge_sd(&in, out);
    }
    return HEALTH_DONE;
}

// --- net-pools -----------------------------------------------------------------------------------

static countcheck_window_t pools_window;    // packet pool, segments, lwIP heap

// lwIP's failure counters are STAT_COUNTER (16 bits unless LWIP_STATS_LARGE): carried into 32-bit
// totals here, so a window across a wrap still counts right. Run at least once per 65536 failures.
static uint32_t pools_total[COUNTCHECK_VALUES];
static STAT_COUNTER pools_last[COUNTCHECK_VALUES];

typedef struct {
    countcheck_pools_t in;
    struct stats_mem pool, seg, heap, pcb;
    struct stats_proto ip, tcp, udp, icmp;
} pools_read_t;

static void pools_gather(pools_read_t *r, countcheck_window_t *win) {
    cyw43_arch_lwip_begin();
    r->pool = *lwip_stats.memp[MEMP_PBUF_POOL];
    r->seg = *lwip_stats.memp[MEMP_TCP_SEG];
    r->pcb = *lwip_stats.memp[MEMP_TCP_PCB];
    r->heap = lwip_stats.mem;
    r->ip = lwip_stats.ip;
    r->tcp = lwip_stats.tcp;
    r->udp = lwip_stats.udp;
    r->icmp = lwip_stats.icmp;
    cyw43_arch_lwip_end();
    STAT_COUNTER raw[COUNTCHECK_VALUES] = {r->pool.err, r->seg.err, r->heap.err};
    for (unsigned i = 0; i < COUNTCHECK_VALUES; i++) {
        pools_total[i] += (STAT_COUNTER)(raw[i] - pools_last[i]);
        pools_last[i] = raw[i];
    }
    uint32_t d[COUNTCHECK_VALUES], span;
    countcheck_window(win, uptime_s(), pools_total, d, &span);
    r->in = (countcheck_pools_t){
        .pbuf = d[0], .seg = d[1], .heap = d[2], .span_s = span,
        .pool_max = r->pool.max, .pool_avail = r->pool.avail,
    };
}

health_step_t countcheck_pools_step(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (!cancel) {
        static pools_read_t r;          // off the stack: eight lwIP stats structures
        pools_gather(&r, &pools_window);
        countcheck_judge_pools(&r.in, out);
    }
    return HEALTH_DONE;
}

#ifdef SOS_TEST_COMMANDS
// --- health fault wifi (test builds) -------------------------------------------------------------

#define WIFI_FAULT_MAX 1000

bool countcheck_board_wifi_fault(const char *kind, const char *count) {
    bool timeout = kind && !strcmp(kind, "timeout");
    if (!timeout && !(kind && !strcmp(kind, "bus")))
        return false;
    size_t len = count ? strlen(count) : 0;
    if (!len || len > 4 || strspn(count, "0123456789") != len)
        return false;
    unsigned long n = strtoul(count, NULL, 10);
    if (n == 0 || n > WIFI_FAULT_MAX)
        return false;
    wifistat_inject(timeout, (unsigned)n);
    return true;
}

// --- health state-dump's phase 6 lines -----------------------------------------------------------

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

static void dump_ctr(health_print_fn print, void *arg, const char *when, const sdcard_ctr_t *c) {
    dump(print, arg, "sd-bus %s: read %llu sectors, written %llu; failed reads %lu, writes %lu; "
         "retried crc %lu, cmd %lu, write %lu; timeouts %lu", when,
         (unsigned long long)c->sectors_read, (unsigned long long)c->sectors_written,
         (unsigned long)c->failed_reads, (unsigned long)c->failed_writes, (unsigned long)c->crc,
         (unsigned long)c->cmd, (unsigned long)c->write, (unsigned long)c->timeout);
}

void countcheck_board_dump(health_print_fn print, void *arg) {
    uint32_t now = uptime_s();

    static wifi_read_t w;
    countcheck_window_t ww = wifi_window;   // a copy: the dump adds no sample
    wifi_gather(&w, &ww);
    dump(print, arg, "wifi: driver %s; errors since boot: timeout %lu, io %lu, bus %lu, tx %lu, "
         "other %lu, held back %lu; last %d in %s, %lu s ago", w.driver,
         (unsigned long)w.ws.timeout, (unsigned long)w.ws.io, (unsigned long)w.ws.bus,
         (unsigned long)w.ws.tx, (unsigned long)w.ws.other, (unsigned long)w.ws.held_back,
         w.ws.last_res, w.ws.last_func ? w.ws.last_func : "-",
         (unsigned long)(w.ws.last_res ? now - w.ws.last_at_s : 0));
    // The down time only means something while the link is down (found on dev-1: "down for
    // 125 s" beside an up link read as a fault); the check judges it only then too.
    static const char *const state_names[] = {"not set up", "stopped", "down", "up"};
    char link[32];
    if (w.in.state == COUNTCHECK_WIFI_DOWN)
        snprintf(link, sizeof(link), "down for %lu s", (unsigned long)w.in.down_s);
    else
        snprintf(link, sizeof(link), "%s",
                 (unsigned)w.in.state < 4 ? state_names[w.in.state] : "?");
    dump(print, arg, "wifi: window %lu s: timeout %lu, io %lu, bus %lu; link %s; "
         "drops since boot %lu, in 24 h %u (last %lu s ago); join rssi %ld dBm",
         (unsigned long)w.in.span_s, (unsigned long)w.in.timeouts, (unsigned long)w.in.io,
         (unsigned long)w.in.bus, link,
         (unsigned long)w.wc.drops, w.in.drops, (unsigned long)w.in.last_drop_ago_s,
         (long)w.wc.join_rssi);
    dump(print, arg, "wifi: chip ver \"%s\"", w.wc.ver);

    countcheck_sd_t sd;
    sdcard_counters_t c;
    sd_gather(&sd, &c);
    dump(print, arg, "sd-bus: inits %lu (failed %lu), ever ok %d, last failed %d, %lu s ago; clock "
         "%lu kHz set, %lu in use; last error %d", (unsigned long)c.inits,
         (unsigned long)c.init_fails, c.ever_ok, c.last_init_failed,
         (unsigned long)(c.inits ? now - c.init_at_s : 0), (unsigned long)c.khz_set,
         (unsigned long)c.khz, c.last_error);
    dump_ctr(print, arg, "since init", &c.since_init);
    dump_ctr(print, arg, "since boot", &c.since_boot);

    static pools_read_t p;
    countcheck_window_t pw = pools_window;
    pools_gather(&p, &pw);
    dump(print, arg, "net-pools: failures since boot: packets %lu, segments %lu, heap %lu, "
         "connections %lu; window %lu s: %lu, %lu, %lu", (unsigned long)p.pool.err,
         (unsigned long)p.seg.err, (unsigned long)p.heap.err, (unsigned long)p.pcb.err,
         (unsigned long)p.in.span_s, (unsigned long)p.in.pbuf, (unsigned long)p.in.seg,
         (unsigned long)p.in.heap);
    dump(print, arg, "net-pools: most/size: packets %lu/%lu, segments %lu/%lu, connections "
         "%lu/%lu, heap %lu/%lu; rx checksum errors: ip %lu, tcp %lu, udp %lu, icmp %lu",
         (unsigned long)p.pool.max, (unsigned long)p.pool.avail, (unsigned long)p.seg.max,
         (unsigned long)p.seg.avail, (unsigned long)p.pcb.max, (unsigned long)p.pcb.avail,
         (unsigned long)p.heap.max, (unsigned long)p.heap.avail, (unsigned long)p.ip.chkerr,
         (unsigned long)p.tcp.chkerr, (unsigned long)p.udp.chkerr, (unsigned long)p.icmp.chkerr);
}
#endif
