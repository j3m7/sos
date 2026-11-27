// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "time/timesync.h"

#include <stdio.h>
#include <sys/time.h>

#include "hardware/sync.h"
#include "pico/aon_timer.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "lwip/apps/sntp.h"

#include "core/log.h"
#include "core/settings.h"
#include "time/nts.h"
#include "time/rtc.h"
#include "time/timeconv.h"

// Set by CMake: the configure time (or SOURCE_DATE_EPOCH). No valid time can be earlier.
#ifndef SOS_BUILD_EPOCH
#error "SOS_BUILD_EPOCH must be defined"
#endif

// NTS time is authenticated, so a periodic sync may step a set clock any distance; steps larger
// than this (an uncalibrated RTC drifts up to ~10 s a day) are logged as warnings.
#define LARGE_STEP_S        300
// Retry delays after a failed automatic NTS sync: doubling from the first to the last.
#define RETRY_FIRST_MS      30000
#define RETRY_MAX_MS        (30 * 60 * 1000)
#define SNTP_TIMEOUT_MS     30000

typedef enum { SRC_NONE, SRC_RTC, SRC_NTS, SRC_SNTP } time_source_t;
typedef enum { WHY_UNSET, WHY_MANUAL, WHY_PERIODIC } sync_reason_t;

static time_source_t source;
static rtc_status_t rtc_boot_status;

// NTS sync.
static bool nts_running;
static sync_reason_t nts_reason;
static bool nts_cancelled;              // periodic sync turned off while running: ignore result
static uint64_t nts_started_us;
static char nts_server_used[CONFIG_NTP_SERVER_MAX + 1];
static uint64_t retry_at_us;            // boot timer; 0 = no retry delay pending
static uint32_t retry_delay_ms;
static uint64_t failed_us;              // boot timer, last failed sync (0 = none since success)
static char failed_why[128];

// Last accepted sync (NTS or SNTP).
static uint64_t synced_us;              // boot timer (0 = never)
// Boot timer, start of the current periodic interval: the last accepted sync, or 0 (boot) if
// none since. Starting at boot means a restart doesn't sync when the RTC had the time: it has
// kept it, and its drift is what the interval is for.
static uint64_t periodic_us;
static int64_t step_ms;
static bool step_was_unset;             // ...which set a clock that had no time
static int64_t synced_delay_us;         // round trip of the NTS sample used

// SNTP, only on `time sync sntp`.
static bool sntp_running;
static uint64_t sntp_started_us;
static char sntp_server[CONFIG_NTP_SERVER_MAX + 1];

// Written from lwIP's context (an interrupt with pico_cyw43_arch_lwip_threadsafe_background),
// consumed by timesync_poll() with interrupts off.
static volatile struct {
    bool ready;
    uint32_t sec, us;
    uint64_t at_us;                     // boot timer when the reply was processed
} sntp_reply;

static int64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

// The SD card library's time() and get_fattime() (no-OS-FatFS my_rtc.c) read the always-on
// timer, not gettimeofday(), and its time() replaces the C library's: wolfSSL checks certificate
// dates with it and FatFs dates files with it. So set both clocks together. Both count the
// crystal, so they stay in step after this.
static void set_clock_us(int64_t t_us) {
    struct timeval tv = {.tv_sec = (time_t)(t_us / 1000000), .tv_usec = (suseconds_t)(t_us % 1000000)};
    settimeofday(&tv, NULL);
    struct timespec ts = {.tv_sec = tv.tv_sec, .tv_nsec = (long)tv.tv_usec * 1000};
    if (aon_timer_is_running())
        aon_timer_set_time(&ts);
    else
        aon_timer_start(&ts);
}

static bool plausible(int64_t t) {
    return t >= SOS_BUILD_EPOCH && t <= 4102444799LL;      // up to 2099-12-31, the RTC's limit
}

static uint64_t periodic_interval_us(void) {
    return (uint64_t)settings()->ntp_sync_hours * 3600 * 1000000;
}

static const char *configured_server(void) {
    return settings()->ntp_server[0] ? settings()->ntp_server : NTS_DEFAULT_SERVER;
}

// "clock set" or "clock stepped +23 ms", for the last accepted sync.
static void format_step(char *buf, size_t len) {
    if (step_was_unset)
        snprintf(buf, len, "clock set");
    else
        snprintf(buf, len, "clock stepped %+lld ms", (long long)step_ms);
}

// Set the system clock and the RTC to t_us (microseconds since 1970).
static void apply_time(int64_t t_us, time_source_t src) {
    step_was_unset = source == SRC_NONE;        // a step from 1970 means nothing
    step_ms = step_was_unset ? 0 : (t_us - now_us()) / 1000;
    set_clock_us(t_us);
    source = src;
    synced_us = periodic_us = time_us_64();     // any accepted sync restarts the interval
    failed_us = 0;

    // Write the RTC on a second boundary so it keeps the sub-second phase (the PCF8523 restarts
    // its divider when the seconds register is written). Waiting is < 1 s.
    int64_t next = (t_us / 1000000 + 1) * 1000000;
    busy_wait_us((uint64_t)(next - t_us));
    bool rtc_ok = rtc_write(next / 1000000);

    char buf[24], step[40];
    timeconv_format(next / 1000000, buf, sizeof(buf));
    format_step(step, sizeof(step));
    log_msg(SOS_LOG_DAEMON | (rtc_ok ? SOS_LOG_NOTICE : SOS_LOG_WARNING), "%s: %s UTC, %s, rtc %s",
            src == SRC_NTS ? "nts" : "sntp", buf, step, rtc_ok ? "updated" : "not updated");
}

// --- NTS -----------------------------------------------------------------------------------------

static bool start_nts(sync_reason_t reason) {
    snprintf(nts_server_used, sizeof(nts_server_used), "%s", configured_server());
    if (!nts_sync_start(nts_server_used))
        return false;                           // busy (e.g. an `nts ke` in progress)
    nts_running = true;
    nts_cancelled = false;
    nts_reason = reason;
    nts_started_us = time_us_64();
    log_msg(SOS_LOG_DAEMON | (reason == WHY_PERIODIC ? SOS_LOG_INFO : SOS_LOG_NOTICE),
            "nts: getting the time from %s%s",
            nts_server_used, reason == WHY_PERIODIC ? " (periodic sync)" : "");
    return true;
}

static void nts_done(void) {
    int64_t offset_us = 0, delay_us = 0;
    const char *err = NULL;
    nts_sync_state_t st = nts_sync_result(&offset_us, &delay_us, &err);
    if (st == NTS_SYNC_RUNNING || st == NTS_SYNC_IDLE)
        return;
    nts_running = false;
    if (nts_cancelled)
        return;

    if (st == NTS_SYNC_OK) {
        int64_t t_us = now_us() + offset_us;
        if (!plausible(t_us / 1000000)) {
            st = NTS_SYNC_FAILED;
            err = "server time earlier than this firmware's build";
        } else {
            if (source != SRC_NONE && (offset_us > LARGE_STEP_S * 1000000LL ||
                                       offset_us < -LARGE_STEP_S * 1000000LL))
                log_msg(SOS_LOG_DAEMON | SOS_LOG_WARNING,
                        "nts: large correction (%+lld s); the RTC was off",
                        (long long)(offset_us / 1000000));
            synced_delay_us = delay_us;
            retry_at_us = 0;
            retry_delay_ms = 0;
            apply_time(t_us, SRC_NTS);
            return;
        }
    }

    // Failed. Retry automatically while the clock has no time, or for a periodic sync; a manual
    // `time sync` on a set clock just reports.
    failed_us = time_us_64();
    snprintf(failed_why, sizeof(failed_why), "%s", err ? err : "?");
    if (source == SRC_NONE || nts_reason == WHY_PERIODIC) {
        retry_delay_ms = retry_delay_ms ? retry_delay_ms * 2 : RETRY_FIRST_MS;
        if (retry_delay_ms > RETRY_MAX_MS)
            retry_delay_ms = RETRY_MAX_MS;
        retry_at_us = time_us_64() + (uint64_t)retry_delay_ms * 1000;
        log_msg(SOS_LOG_DAEMON | SOS_LOG_WARNING, "nts: sync with %s failed: %s; retrying in %lu s",
                nts_server_used, failed_why, (unsigned long)(retry_delay_ms / 1000));
    } else {
        log_msg(SOS_LOG_DAEMON | SOS_LOG_WARNING,
                "nts: sync with %s failed: %s", nts_server_used, failed_why);
    }
}

// --- SNTP (only on request) ----------------------------------------------------------------------

// lwIP SNTP hooks (lwipopts.h); lwIP context.
void sos_sntp_set_time(unsigned int sec, unsigned int us) {
    sntp_reply.sec = sec;
    sntp_reply.us = us;
    sntp_reply.at_us = time_us_64();
    __dmb();
    sntp_reply.ready = true;
}

// The local clock, used for the request's transmit timestamp (which the reply must echo) and
// for round-trip compensation.
void sos_sntp_get_time(unsigned int *sec, unsigned int *us) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    *sec = (unsigned int)tv.tv_sec;
    *us = (unsigned int)tv.tv_usec;
}

static void sntp_begin(void) {
    // lwIP keeps the pointer: copied here so it stays valid for the whole sync.
    snprintf(sntp_server, sizeof(sntp_server), "%s", SOS_NTP_SERVER);
    cyw43_arch_lwip_begin();
    if (!sntp_enabled()) {
        sntp_setoperatingmode(SNTP_OPMODE_POLL);
        sntp_setservername(0, sntp_server);
        sntp_init();
    }
    cyw43_arch_lwip_end();
    sntp_running = true;
    sntp_started_us = time_us_64();
    sntp_reply.ready = false;               // a reply left over from an earlier run doesn't count
    log_msg(SOS_LOG_DAEMON | SOS_LOG_NOTICE,
            "sntp: requesting time from %s (unauthenticated)", sntp_server);
}

static void sntp_end(void) {
    cyw43_arch_lwip_begin();
    sntp_stop();
    cyw43_arch_lwip_end();
    sntp_running = false;
}

static void sntp_done(void) {
    if (time_us_64() - sntp_started_us > (uint64_t)SNTP_TIMEOUT_MS * 1000) {
        sntp_end();
        log_msg(SOS_LOG_DAEMON | SOS_LOG_WARNING, "sntp: no reply from %s within %d s", sntp_server,
                SNTP_TIMEOUT_MS / 1000);
        return;
    }
    if (!sntp_reply.ready)
        return;
    uint32_t irq = save_and_disable_interrupts();
    int64_t reply_us = (int64_t)sntp_reply.sec * 1000000 + sntp_reply.us;
    uint64_t at_us = sntp_reply.at_us;
    sntp_reply.ready = false;
    restore_interrupts(irq);

    int64_t t_us = reply_us + (int64_t)(time_us_64() - at_us);
    if (!plausible(t_us / 1000000)) {
        char buf[24];
        timeconv_format(t_us / 1000000, buf, sizeof(buf));
        log_msg(SOS_LOG_DAEMON | SOS_LOG_WARNING, "sntp: rejected implausible time %s UTC", buf);
        return;                                 // lwIP asks again after SNTP_UPDATE_DELAY
    }
    sntp_end();
    apply_time(t_us, SRC_SNTP);
}

// --- public --------------------------------------------------------------------------------------

void timesync_boot(void) {
    if (!rtc_init()) {
        rtc_boot_status = RTC_ABSENT;
        log_msg(SOS_LOG_DAEMON | SOS_LOG_WARNING, rtc_bus_pulled_up()
                ? "rtc: no answer on I2C (GP4/GP5, 0x68); the time will come from NTS"
                : "rtc: not found (no I2C pull-ups on GP4/GP5: Adalogger not attached?); "
                  "the time will come from NTS");
        return;
    }
    int64_t t;
    rtc_boot_status = rtc_read(&t);
    if (rtc_boot_status == RTC_OK && !plausible(t))
        rtc_boot_status = RTC_INVALID;
    if (rtc_boot_status != RTC_OK) {
        log_msg(SOS_LOG_DAEMON | SOS_LOG_WARNING, "rtc: %s; the time will come from NTS",
                rtc_status_str(rtc_boot_status));
        return;
    }
    set_clock_us(t * 1000000);
    source = SRC_RTC;
    char buf[24];
    timeconv_format(t, buf, sizeof(buf));
    log_msg(SOS_LOG_DAEMON | SOS_LOG_NOTICE, "rtc: %s UTC", buf);
    if (rtc_battery_low())
        log_msg(SOS_LOG_DAEMON | SOS_LOG_WARNING, "rtc: backup battery low; replace the CR1220");
}

_Static_assert(CONFIG_RTC_OFFSET_MIN == TIMECONV_CAL_STEPS_MIN &&
               CONFIG_RTC_OFFSET_MAX == TIMECONV_CAL_STEPS_MAX, "the setting holds register steps");

void timesync_cal_str(char *buf, unsigned len) {
    int steps = settings()->rtc_offset;
    if (!steps) {
        snprintf(buf, len, "off");
        return;
    }
    long mppm = (long)(steps < 0 ? -steps : steps) * TIMECONV_CAL_STEP_MPPM;
    snprintf(buf, len, "%s by %ld.%ld ppm", steps > 0 ? "slowed" : "sped up", mppm / 1000,
             mppm % 1000 / 100);
}

bool timesync_apply_cal(void) {
    int want = settings()->rtc_offset, have;
    if (rtc_read_offset(&have) && have == want)
        return true;
    if (rtc_boot_status == RTC_ABSENT)
        return false;                           // already reported at boot
    char cal[40];
    timesync_cal_str(cal, sizeof(cal));
    if (!rtc_write_offset(want)) {
        log_msg(SOS_LOG_DAEMON | SOS_LOG_WARNING, "rtc: setting the calibration failed");
        return false;
    }
    log_msg(SOS_LOG_DAEMON | SOS_LOG_NOTICE, "rtc: calibration %s", cal);
    return true;
}

bool timesync_clock_set(void) {
    return source != SRC_NONE;
}

void timesync_network_up(void) {
    if (nts_running || sntp_running)
        return;
    if (retry_at_us && time_us_64() < retry_at_us)
        return;
    if (source == SRC_NONE) {
        start_nts(WHY_UNSET);
        return;
    }
    // Periodic sync: one interval after the last accepted sync, or after boot if there was none
    // (the clock then came from the RTC). Not at boot: restarts would each ask the server for a
    // time the RTC already has.
    uint64_t interval = periodic_interval_us();
    if (interval && time_us_64() - periodic_us >= interval)
        start_nts(WHY_PERIODIC);
}

bool timesync_request_nts(void) {
    if (sntp_running)
        return false;
    if (nts_running) {
        if (nts_reason != WHY_PERIODIC)
            return false;
        nts_reason = WHY_MANUAL;                // take over a periodic sync in progress
        nts_cancelled = false;
        return true;
    }
    retry_at_us = 0;                            // a manual request skips any retry delay
    return start_nts(WHY_MANUAL);
}

bool timesync_request_sntp(void) {
    if (nts_running || sntp_running)
        return false;
    sntp_begin();
    return true;
}

void timesync_poll(void) {
    if (nts_running) {
        // Periodic sync turned off while it runs: let it finish, but don't use the result.
        if (nts_reason == WHY_PERIODIC && !settings()->ntp_sync_hours)
            nts_cancelled = true;
        nts_done();
    }
    if (sntp_running)
        sntp_done();
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
    static const char *const names[] = {"not set", "from rtc", "from nts, authenticated",
                                         "from sntp, unauthenticated"};
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

    // The saved calibration, and whether the RTC has it (it can't if the chip is absent).
    char cal[40];
    int steps, want = settings()->rtc_offset;
    timesync_cal_str(cal, sizeof(cal));
    bool applied = rtc_read_offset(&steps) ? steps == want : want == 0;
    shell_printf(sh, "cal:    %s%s\n", cal, applied ? "" : " (not in the RTC)");

    uint64_t now = time_us_64();
    shell_printf(sh, "sync:   ");
    if (nts_running) {
        shell_printf(sh, "running with %s (", nts_server_used);
        print_duration(sh, now - nts_started_us);
        shell_printf(sh, ")");
    } else if (sntp_running) {
        shell_printf(sh, "sntp running with %s (", sntp_server);
        print_duration(sh, now - sntp_started_us);
        shell_printf(sh, ")");
    } else if (synced_us) {
        shell_printf(sh, "%s ", source == SRC_SNTP ? "sntp" : "nts");
        print_duration(sh, now - synced_us);
        char step[40];
        format_step(step, sizeof(step));
        shell_printf(sh, " ago, %s", step);
        if (source == SRC_NTS)
            shell_printf(sh, ", delay %lld ms", (long long)(synced_delay_us / 1000));
    } else {
        shell_printf(sh, "not synced since boot");
    }
    shell_printf(sh, "\n");
    if (failed_us) {
        shell_printf(sh, "failed: ");
        print_duration(sh, now - failed_us);
        shell_printf(sh, " ago: %s", failed_why);
        if (retry_at_us && retry_at_us > now) {
            shell_printf(sh, "; retry in ");
            print_duration(sh, retry_at_us - now);
        }
        shell_printf(sh, "\n");
    }
    shell_printf(sh, "server: %s (NTS)%s\n", configured_server(),
                 settings()->ntp_server[0] ? "" : ", build default");

    uint64_t interval = periodic_interval_us();
    if (!interval) {
        shell_printf(sh, "auto:   off\n");
    } else {
        shell_printf(sh, "auto:   every %u h", (unsigned)settings()->ntp_sync_hours);
        if (!nts_running) {
            uint64_t elapsed = now - periodic_us;
            shell_printf(sh, ", next in ");
            print_duration(sh, elapsed < interval ? interval - elapsed : 0);
        }
        shell_printf(sh, "\n");
    }
}
