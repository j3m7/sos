// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "timesync.h"

#include <sys/time.h>

#include "hardware/sync.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "lwip/apps/sntp.h"

#include "log.h"
#include "rtc.h"
#include "timeconv.h"

// Set by CMake: the configure time (or SOURCE_DATE_EPOCH). No valid time can be earlier.
#ifndef SOS_BUILD_EPOCH
#error "SOS_BUILD_EPOCH must be defined"
#endif

typedef enum { SRC_NONE, SRC_RTC, SRC_NTP } time_source_t;

static time_source_t source;
static rtc_status_t rtc_boot_status;
static bool ntp_running;
static uint64_t ntp_started_us;         // boot timer, when SNTP was started
static uint64_t ntp_synced_us;          // boot timer, last accepted reply (0 = never)
static int64_t ntp_step_ms;             // clock change made by that reply
static uint32_t ntp_rejected;           // replies refused as implausible

// Written from lwIP's context (an interrupt with pico_cyw43_arch_lwip_threadsafe_background),
// consumed by timesync_poll() with interrupts off.
static volatile struct {
    bool ready;
    uint32_t sec, us;
    uint64_t at_us;                     // boot timer when the reply was processed
} ntp_reply;

static int64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static void set_clock_us(int64_t t_us) {
    struct timeval tv = {.tv_sec = (time_t)(t_us / 1000000), .tv_usec = (suseconds_t)(t_us % 1000000)};
    settimeofday(&tv, NULL);
}

static bool plausible(int64_t t) {
    return t >= SOS_BUILD_EPOCH && t <= 4102444799LL;      // up to 2099-12-31, the RTC's limit
}

// --- lwIP SNTP hooks (lwipopts.h); lwIP context ----------------------------------------------

void sos_sntp_set_time(unsigned int sec, unsigned int us) {
    ntp_reply.sec = sec;
    ntp_reply.us = us;
    ntp_reply.at_us = time_us_64();
    __dmb();
    ntp_reply.ready = true;
}

// The local clock, used for the request's transmit timestamp (which the reply must echo) and
// for round-trip compensation.
void sos_sntp_get_time(unsigned int *sec, unsigned int *us) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    *sec = (unsigned int)tv.tv_sec;
    *us = (unsigned int)tv.tv_usec;
}

// --- SNTP control ------------------------------------------------------------------------------

static void ntp_start(void) {
    cyw43_arch_lwip_begin();
    if (!sntp_enabled()) {
        sntp_setoperatingmode(SNTP_OPMODE_POLL);
        sntp_setservername(0, SOS_NTP_SERVER);
        sntp_init();
    }
    cyw43_arch_lwip_end();
    ntp_running = true;
    ntp_started_us = time_us_64();
    log_msg(SOS_LOG_NOTICE, "ntp: requesting time from %s", SOS_NTP_SERVER);
}

static void ntp_stop(void) {
    cyw43_arch_lwip_begin();
    sntp_stop();
    cyw43_arch_lwip_end();
    ntp_running = false;
}

// --- public ------------------------------------------------------------------------------------

void timesync_boot(void) {
    if (!rtc_init()) {
        rtc_boot_status = RTC_ABSENT;
        log_msg(SOS_LOG_WARNING, "rtc: not found; the time will come from NTP");
        return;
    }
    int64_t t;
    rtc_boot_status = rtc_read(&t);
    if (rtc_boot_status == RTC_OK && !plausible(t))
        rtc_boot_status = RTC_INVALID;
    if (rtc_boot_status != RTC_OK) {
        log_msg(SOS_LOG_WARNING, "rtc: %s; the time will come from NTP",
                rtc_status_str(rtc_boot_status));
        return;
    }
    set_clock_us(t * 1000000);
    source = SRC_RTC;
    char buf[24];
    timeconv_format(t, buf, sizeof(buf));
    log_msg(SOS_LOG_NOTICE, "rtc: %s UTC", buf);
    if (rtc_battery_low())
        log_msg(SOS_LOG_WARNING, "rtc: backup battery low; replace the CR1220");
}

void timesync_network_up(void) {
    if (source == SRC_NONE && !ntp_running)
        ntp_start();
}

bool timesync_request_ntp(void) {
    if (ntp_running)
        return false;
    ntp_start();
    return true;
}

void timesync_poll(void) {
    if (!ntp_reply.ready)
        return;
    uint32_t irq = save_and_disable_interrupts();
    int64_t reply_us = (int64_t)ntp_reply.sec * 1000000 + ntp_reply.us;
    uint64_t at_us = ntp_reply.at_us;
    ntp_reply.ready = false;
    restore_interrupts(irq);

    if (!plausible(reply_us / 1000000)) {
        // Keep SNTP running: lwIP asks again after SNTP_UPDATE_DELAY.
        ntp_rejected++;
        char buf[24];
        timeconv_format(reply_us / 1000000, buf, sizeof(buf));
        log_msg(SOS_LOG_WARNING, "ntp: rejected implausible time %s", buf);
        return;
    }
    ntp_stop();

    int64_t t_us = reply_us + (int64_t)(time_us_64() - at_us);
    ntp_step_ms = (t_us - now_us()) / 1000;
    set_clock_us(t_us);
    source = SRC_NTP;
    ntp_synced_us = time_us_64();

    // Write the RTC on a second boundary so it keeps the sub-second phase (the PCF8523 restarts
    // its divider when the seconds register is written). Waiting is < 1 s.
    int64_t next = (t_us / 1000000 + 1) * 1000000;
    busy_wait_us((uint64_t)(next - t_us));
    bool rtc_ok = rtc_write(next / 1000000);

    char buf[24];
    timeconv_format(next / 1000000, buf, sizeof(buf));
    log_msg(rtc_ok ? SOS_LOG_NOTICE : SOS_LOG_WARNING,
            "ntp: %s UTC, clock stepped %+lld ms, rtc %s", buf, (long long)ntp_step_ms,
            rtc_ok ? "updated" : "not updated");
}

static void print_ago(shell_t *sh, uint64_t since_us) {
    uint64_t s = (time_us_64() - since_us) / 1000000;
    if (s < 120)
        shell_printf(sh, "%llus", (unsigned long long)s);
    else if (s < 7200)
        shell_printf(sh, "%llum", (unsigned long long)(s / 60));
    else if (s < 172800)
        shell_printf(sh, "%lluh", (unsigned long long)(s / 3600));
    else
        shell_printf(sh, "%llud", (unsigned long long)(s / 86400));
}

void timesync_status(shell_t *sh) {
    char buf[24];
    static const char *const names[] = {"not set", "from rtc", "from ntp"};
    if (source == SRC_NONE) {
        shell_printf(sh, "time:  not set\n");
    } else {
        timeconv_format(now_us() / 1000000, buf, sizeof(buf));
        shell_printf(sh, "time:  %s UTC (%s)\n", buf, names[source]);
    }

    int64_t t;
    rtc_status_t st = rtc_read(&t);
    if (st == RTC_OK) {
        timeconv_format(t, buf, sizeof(buf));
        shell_printf(sh, "rtc:   %s UTC%s\n", buf, rtc_battery_low() ? ", backup battery low" : "");
    } else {
        shell_printf(sh, "rtc:   %s\n", rtc_status_str(st));
    }

    shell_printf(sh, "ntp:   ");
    if (ntp_running) {
        shell_printf(sh, "waiting for %s (", SOS_NTP_SERVER);
        print_ago(sh, ntp_started_us);
        shell_printf(sh, ")");
    } else if (ntp_synced_us) {
        shell_printf(sh, "synced ");
        print_ago(sh, ntp_synced_us);
        shell_printf(sh, " ago, stepped %+lld ms", (long long)ntp_step_ms);
    } else {
        shell_printf(sh, "not used (rtc set at boot; 'time sync' to sync now)");
    }
    if (ntp_rejected)
        shell_printf(sh, ", %lu rejected", (unsigned long)ntp_rejected);
    shell_printf(sh, "\n");
}
