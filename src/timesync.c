// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "timesync.h"

#include <stdio.h>
#include <sys/time.h>

#include "hardware/sync.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "lwip/apps/sntp.h"

#include "log.h"
#include "rtc.h"
#include "settings.h"
#include "timeconv.h"

// Set by CMake: the configure time (or SOURCE_DATE_EPOCH). No valid time can be earlier.
#ifndef SOS_BUILD_EPOCH
#error "SOS_BUILD_EPOCH must be defined"
#endif

// A periodic sync only corrects drift (up to ~10 s a week): a reply that would move a set clock
// further than this is refused, so a forged reply can't silently move it. `time sync` accepts
// any plausible time.
#define PERIODIC_MAX_STEP_S 300

typedef enum { SRC_NONE, SRC_RTC, SRC_NTP } time_source_t;

// Why SNTP is running; decides which replies are accepted.
typedef enum {
    NTP_UNSET,          // the clock has no time: accept any plausible reply
    NTP_MANUAL,         // `time sync`: likewise
    NTP_PERIODIC,       // ntp_sync_hours: accept small corrections only
} ntp_reason_t;

static time_source_t source;
static rtc_status_t rtc_boot_status;
static bool ntp_running;
static ntp_reason_t ntp_reason;
static uint64_t ntp_started_us;         // boot timer, when SNTP was started
static uint64_t ntp_synced_us;          // boot timer, last accepted reply (0 = never)
static uint64_t ntp_periodic_us;        // boot timer, when the last periodic sync ended (0 = none)
static int64_t ntp_step_ms;             // clock change made by the last accepted reply
static bool ntp_was_unset;              // ...which set a clock that had no time
static uint32_t ntp_rejected;           // replies refused

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

// "clock set" or "clock stepped +23 ms", for the last accepted reply.
static void format_step(char *buf, size_t len) {
    if (ntp_was_unset)
        snprintf(buf, len, "clock set");
    else
        snprintf(buf, len, "clock stepped %+lld ms", (long long)ntp_step_ms);
}

static uint64_t periodic_interval_us(void) {
    return (uint64_t)settings()->ntp_sync_hours * 3600 * 1000000;
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

// The server for the current (or last) sync. lwIP keeps the pointer, so it's copied here rather
// than pointing into the settings, which may change mid-sync.
static char ntp_server[CONFIG_NTP_SERVER_MAX + 1];

static const char *configured_server(void) {
    return settings()->ntp_server[0] ? settings()->ntp_server : SOS_NTP_SERVER;
}

static void ntp_start(ntp_reason_t reason) {
    cyw43_arch_lwip_begin();
    if (!sntp_enabled()) {
        snprintf(ntp_server, sizeof(ntp_server), "%s", configured_server());
        sntp_setoperatingmode(SNTP_OPMODE_POLL);
        sntp_setservername(0, ntp_server);
        sntp_init();
    }
    cyw43_arch_lwip_end();
    ntp_running = true;
    ntp_reason = reason;
    ntp_started_us = time_us_64();
    ntp_reply.ready = false;            // a reply left over from an earlier run doesn't count
    log_msg(reason == NTP_PERIODIC ? SOS_LOG_INFO : SOS_LOG_NOTICE,
            "ntp: requesting time from %s%s", ntp_server,
            reason == NTP_PERIODIC ? " (periodic sync)" : "");
}

static void ntp_stop(void) {
    cyw43_arch_lwip_begin();
    sntp_stop();
    cyw43_arch_lwip_end();
    if (ntp_reason == NTP_PERIODIC)
        ntp_periodic_us = time_us_64();
    ntp_running = false;
}

// --- public ------------------------------------------------------------------------------------

void timesync_boot(void) {
    if (!rtc_init()) {
        rtc_boot_status = RTC_ABSENT;
        log_msg(SOS_LOG_WARNING, rtc_bus_pulled_up()
                ? "rtc: no answer on I2C (GP4/GP5, 0x68); the time will come from NTP"
                : "rtc: not found (no I2C pull-ups on GP4/GP5: Adalogger not attached?); "
                  "the time will come from NTP");
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
    if (ntp_running)
        return;
    if (source == SRC_NONE) {
        ntp_start(NTP_UNSET);
        return;
    }
    // Periodic sync: the first one soon after boot (or after it's turned on), then every
    // interval after the previous one ended.
    uint64_t interval = periodic_interval_us();
    if (interval && (!ntp_periodic_us || time_us_64() - ntp_periodic_us >= interval))
        ntp_start(NTP_PERIODIC);
}

bool timesync_request_ntp(void) {
    if (ntp_running) {
        if (ntp_reason != NTP_PERIODIC)
            return false;
        ntp_reason = NTP_MANUAL;        // take over a periodic sync in progress
        return true;
    }
    ntp_start(NTP_MANUAL);
    return true;
}

static void reject(const char *why, int64_t t) {
    ntp_rejected++;
    char buf[24];
    timeconv_format(t, buf, sizeof(buf));
    log_msg(SOS_LOG_WARNING, "ntp: rejected %s (%s UTC)%s", why, buf,
            ntp_reason == NTP_PERIODIC ? "; 'time sync' accepts it" : "");
    // Clock unset or `time sync`: keep SNTP running, lwIP asks again after SNTP_UPDATE_DELAY.
    // Periodic: give up until the next interval.
    if (ntp_reason == NTP_PERIODIC)
        ntp_stop();
}

void timesync_poll(void) {
    // Periodic sync turned off while one is running.
    if (ntp_running && ntp_reason == NTP_PERIODIC && !settings()->ntp_sync_hours) {
        ntp_stop();
        ntp_periodic_us = 0;
    }

    if (!ntp_reply.ready)
        return;
    uint32_t irq = save_and_disable_interrupts();
    int64_t reply_us = (int64_t)ntp_reply.sec * 1000000 + ntp_reply.us;
    uint64_t at_us = ntp_reply.at_us;
    ntp_reply.ready = false;
    restore_interrupts(irq);
    if (!ntp_running)
        return;

    int64_t t_us = reply_us + (int64_t)(time_us_64() - at_us);
    int64_t step_us = t_us - now_us();
    if (!plausible(t_us / 1000000)) {
        reject("implausible time", t_us / 1000000);
        return;
    }
    if (ntp_reason == NTP_PERIODIC &&
        (step_us > PERIODIC_MAX_STEP_S * 1000000LL || step_us < -PERIODIC_MAX_STEP_S * 1000000LL)) {
        reject("a step of more than 5 minutes", t_us / 1000000);
        return;
    }
    ntp_stop();

    // A step from an unset clock (counting from 1970) means nothing; report it as "set".
    ntp_was_unset = source == SRC_NONE;
    ntp_step_ms = ntp_was_unset ? 0 : step_us / 1000;
    set_clock_us(t_us);
    source = SRC_NTP;
    ntp_synced_us = time_us_64();
    ntp_periodic_us = ntp_synced_us;    // any accepted sync restarts the periodic interval

    // Write the RTC on a second boundary so it keeps the sub-second phase (the PCF8523 restarts
    // its divider when the seconds register is written). Waiting is < 1 s.
    int64_t next = (t_us / 1000000 + 1) * 1000000;
    busy_wait_us((uint64_t)(next - t_us));
    bool rtc_ok = rtc_write(next / 1000000);

    char buf[24], step[40];
    timeconv_format(next / 1000000, buf, sizeof(buf));
    format_step(step, sizeof(step));
    log_msg(rtc_ok ? SOS_LOG_NOTICE : SOS_LOG_WARNING, "ntp: %s UTC, %s, rtc %s", buf, step,
            rtc_ok ? "updated" : "not updated");
}

static void print_duration(shell_t *sh, uint64_t us) {
    uint64_t s = us / 1000000;
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
        shell_printf(sh, "time:   not set\n");
    } else {
        timeconv_format(now_us() / 1000000, buf, sizeof(buf));
        shell_printf(sh, "time:   %s UTC (%s)\n", buf, names[source]);
    }

    int64_t t;
    rtc_status_t st = rtc_read(&t);
    if (st == RTC_OK) {
        timeconv_format(t, buf, sizeof(buf));
        shell_printf(sh, "rtc:    %s UTC%s\n", buf, rtc_battery_low() ? ", backup battery low" : "");
    } else {
        shell_printf(sh, "rtc:    %s\n", rtc_status_str(st));
    }

    uint64_t now = time_us_64();
    shell_printf(sh, "ntp:    ");
    if (ntp_running) {
        shell_printf(sh, "waiting for %s (", ntp_server);
        print_duration(sh, now - ntp_started_us);
        shell_printf(sh, ")");
    } else if (ntp_synced_us) {
        shell_printf(sh, "synced ");
        print_duration(sh, now - ntp_synced_us);
        char step[40];
        format_step(step, sizeof(step));
        shell_printf(sh, " ago, %s", step);
    } else {
        shell_printf(sh, "not synced since boot");
    }
    if (ntp_rejected)
        shell_printf(sh, ", %lu rejected", (unsigned long)ntp_rejected);
    shell_printf(sh, "\n");
    shell_printf(sh, "server: %s%s\n", configured_server(),
                 settings()->ntp_server[0] ? "" : " (build default)");

    uint64_t interval = periodic_interval_us();
    if (!interval) {
        shell_printf(sh, "auto:   off\n");
    } else {
        shell_printf(sh, "auto:   every %u h", (unsigned)settings()->ntp_sync_hours);
        if (!ntp_running && ntp_periodic_us) {
            uint64_t elapsed = now - ntp_periodic_us;
            shell_printf(sh, ", next in ");
            print_duration(sh, elapsed < interval ? interval - elapsed : 0);
        }
        shell_printf(sh, "\n");
    }
}
