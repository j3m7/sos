// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See wifistat.h. The two hooks config/cyw43_sos.h gives the driver: counting, and the driver's
// warnings into the log (never printf, which would print them raw on the serial console, even
// while it is locked).

#include "net/wifistat.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#include "hardware/sync.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"

#include "core/countcheck.h"
#include "core/log.h"

#ifndef SOS_CYW43_HOOKS
#error "the CYW43 driver isn't configured by config/cyw43_sos.h (CYW43_CONFIG_FILE, CMakeLists.txt)"
#endif

// Written from the driver's background (interrupt) context and from callers holding its lock,
// all on core 0; read with interrupts off, so a copy is one moment's.
static wifistat_t st;

static void count_fail(int res, const char *func) {
    if (res == -CYW43_ETIMEDOUT)
        st.timeout++;
    else if (res == -CYW43_EIO)
        st.io++;
    else
        st.other++;
    st.last_res = res;
    st.last_func = func;
    st.last_at_s = (uint32_t)(time_us_64() / 1000000);
}

int wifistat_fail(int res, const char *func) {
    if (res < 0) {
        uint32_t saved = save_and_disable_interrupts();
        count_fail(res, func);
        restore_interrupts(saved);
    }
    return res;
}

// --- warnings ------------------------------------------------------------------------------------

#define SITES        8                  // warning sites remembered for the rate limit
#define SITE_GAP_US  (10u * 1000000)    // one line per site this often

static struct {
    const char *func;
    int line;
    uint64_t last_us;                   // when it was last logged (0: never)
    uint32_t held;                      // lines held back since
} sites[SITES];

// The site's slot, or a new one in place of the least recently logged. Interrupts off.
static unsigned site_of(const char *func, int line) {
    unsigned oldest = 0;
    for (unsigned i = 0; i < SITES; i++) {
        if (sites[i].func == func && sites[i].line == line)
            return i;
        if (sites[i].last_us < sites[oldest].last_us)
            oldest = i;
    }
    sites[oldest].func = func;
    sites[oldest].line = line;
    sites[oldest].last_us = 0;
    sites[oldest].held = 0;
    return oldest;
}

void wifistat_warn(const char *func, int line, const char *fmt, ...) {
    uint64_t now = time_us_64();
    uint32_t saved = save_and_disable_interrupts();
    switch (countcheck_wifi_warn_kind(func)) {
    case COUNTCHECK_WIFI_BUS:   st.bus++;   break;
    case COUNTCHECK_WIFI_TX:    st.tx++;    break;
    case COUNTCHECK_WIFI_OTHER: st.other++; break;
    case COUNTCHECK_WIFI_NONE:  break;      // counted by its error return
    }
    unsigned i = site_of(func, line);
    bool log = !sites[i].last_us || now - sites[i].last_us >= SITE_GAP_US;
    uint32_t held = sites[i].held;
    if (log) {
        sites[i].last_us = now ? now : 1;
        sites[i].held = 0;
    } else {
        sites[i].held++;
        st.held_back++;
    }
    restore_interrupts(saved);
    if (!log)
        return;

    // log_msg() only stores a line logged in interrupt context; the main loop prints it.
    char prefix[48];
    if (held)
        snprintf(prefix, sizeof(prefix), "wifi: driver (%lu more held back): ",
                 (unsigned long)held);
    else
        snprintf(prefix, sizeof(prefix), "wifi: driver: ");
    va_list ap;
    va_start(ap, fmt);
    log_vmsg_prefixed(SOS_LOG_DAEMON | SOS_LOG_WARNING, prefix, fmt, ap);
    va_end(ap);
}

void wifistat_get(wifistat_t *out) {
    uint32_t saved = save_and_disable_interrupts();
    *out = st;
    restore_interrupts(saved);
}

#ifdef SOS_TEST_COMMANDS
// As the driver's own sites would: a timeout as cyw43_do_ioctl() returns one, a bus error as
// cyw43_ll_sdpcm_poll_device() warns of one (rate-limited like it).
void wifistat_inject(bool timeout, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        if (timeout)
            wifistat_fail(-CYW43_ETIMEDOUT, "cyw43_do_ioctl");
        else
            wifistat_warn("cyw43_ll_sdpcm_poll_device", 0,
                          "Bus error condition detected 0x%x (health fault)", 0u);
    }
}
#endif
