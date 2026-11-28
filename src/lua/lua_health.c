// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Core 0's answers to LR_INFO: the read-only tables of sos.sensors, sos.mem, sos.version,
// sos.reset_reason, sos.glitch, sos.wifi.status, sos.dmesg and sos.health, written with
// lua_info.h. Each reads what the matching shell command (temp and power, mem, version, uptime,
// glitch, net and wifi, dmesg, health) shows, and all of it may be shown over SSH. None of it waits: the sensors' latest readings are used (taken
// every SENSORS_SAMPLE_S by the main loop), not a new reading.
//
// A field the board doesn't know is left out, so the script sees nil: the temp() and volts()
// helpers skip SENSORS_NONE, and lua_info_str() writes nothing for a NULL string (wifi_ssid()
// when not connected, fault_last() when there was no fault).

#include "lua/lua_health.h"
#include "lua/lua_health_table.h"

#include <string.h>
#include <sys/time.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "lwip/netif.h"
#include "lwip/stats.h"

#include "core/chipinfo.h"
#include "core/config.h"
#include "core/consoleonly.h"
#include "core/countcheck.h"
#include "core/fault.h"
#include "core/glitchdet.h"
#include "core/health.h"
#include "core/heap.h"
#include "core/heaptag.h"
#include "core/log.h"
#include "core/otpread.h"
#include "core/resetwhy.h"
#include "core/rootsource.h"
#include "core/sensors.h"
#include "core/settings.h"
#include "core/stackmon.h"
#include "core/version.h"
#include "core/wdog.h"
#include "chat/chat.h"
#include "files/sdcard.h"
#include "ssh/sshd.h"
#include "tls/certstore.h"
#include "tls/tls_roots.h"
#include "lua/lua_core1.h"
#include "net/wifi.h"
#include "net/wifistat.h"
#include "time/rtc.h"
#include "time/timesync.h"

static void temp(lua_info_t *w, const char *name, int tenths) {
    if (tenths != SENSORS_NONE)
        lua_info_num(w, name, tenths / 10.0);
}

static void volts(lua_info_t *w, const char *name, int mv) {
    if (mv != SENSORS_NONE)
        lua_info_num(w, name, mv / 1000.0);
}

static void sensors_table(lua_info_t *w) {
    temp(w, "temp", sensors_last_temp());
    const sensors_stats_t *t = sensors_temp_stats();
    if (t->count) {
        temp(w, "temp_min", t->min);
        temp(w, "temp_max", t->max);
    }
    lua_info_bool(w, "temp_calibrated", settings()->temp_offset != 0);
    volts(w, "vsys", sensors_last_vsys());
    const sensors_stats_t *v = sensors_vsys_stats();
    if (v->count) {
        volts(w, "vsys_min", v->min);
        volts(w, "vsys_max", v->max);
    }
    int usb = sensors_usb_power();
    if (usb >= 0)
        lua_info_bool(w, "usb", usb > 0);
}

static void used_peak_total(lua_info_t *w, const char *name, size_t used, size_t peak,
                            size_t total) {
    lua_info_open(w, name);
    lua_info_int(w, "used", (int64_t)used);
    lua_info_int(w, "peak", (int64_t)peak);
    lua_info_int(w, "total", (int64_t)total);
    lua_info_close(w);
}

// The script's own allocation is added by core 1, which keeps it.
static void mem_table(lua_info_t *w) {
    heap_info_t h = heap_info();
    used_peak_total(w, "heap", h.used, h.arena, h.total);
    lua_info_open(w, "tags");
    for (int t = 0; t < HEAP_TAGS; t++) {
        lua_info_open(w, heap_tag_name((heap_tag_t)t));
        lua_info_int(w, "used", (int64_t)heap_tag_used((heap_tag_t)t));
        lua_info_int(w, "peak", (int64_t)heap_tag_peak((heap_tag_t)t));
        lua_info_close(w);
    }
    lua_info_close(w);
    cyw43_arch_lwip_begin();
    struct stats_mem net = lwip_stats.mem;
    unsigned pool_err = lwip_stats.memp[MEMP_PBUF_POOL]->err;
    unsigned seg_err = lwip_stats.memp[MEMP_TCP_SEG]->err;
    cyw43_arch_lwip_end();
    lua_info_open(w, "lwip");
    lua_info_int(w, "used", (int64_t)net.used);
    lua_info_int(w, "peak", (int64_t)net.max);
    lua_info_int(w, "total", (int64_t)net.avail);
    // What the `net-pools` check judges, since boot (lwIP's counters, which wrap at 65536).
    lua_info_open(w, "failures");
    lua_info_int(w, "packets", pool_err);
    lua_info_int(w, "segments", seg_err);
    lua_info_int(w, "heap", net.err);
    lua_info_close(w);
    lua_info_close(w);
    lua_info_open(w, "stack");
    lua_info_int(w, "peak", (int64_t)stackmon_peak());
    lua_info_int(w, "size", (int64_t)stackmon_size());
    lua_info_close(w);
    lua_info_open(w, "core1_stack");
    lua_info_int(w, "peak", (int64_t)lua_core1_stack_peak());
    lua_info_int(w, "size", LUA_CORE1_STACK);
    lua_info_close(w);
}

static void version_table(lua_info_t *w) {
    char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1], chip[64];
    pico_get_unique_board_id_string(serial, sizeof(serial));
    chipinfo_format(chip, sizeof(chip), rp2350_chip_version(), rp2350_rom_version());
    lua_info_str(w, "firmware", sos_version);
    lua_info_str(w, "serial", serial);
    lua_info_str(w, "chip", chip);
    lua_info_int(w, "chip_revision", rp2350_chip_version());
    lua_info_int(w, "rom_version", rp2350_rom_version());
    otpread_status_t otp;
    otpread_status(&otp);
    lua_info_bool(w, "secure_boot", otp.secure_boot);
    lua_info_str(w, "debug", otp.debug_disabled ? "disabled"
                 : otp.secure_debug_disabled ? "secure disabled" : "enabled");
    lua_info_str(w, "root_secret", rootsource() == ROOTSRC_OTP ? "otp"
                 : rootsource() == ROOTSRC_FLASH ? "flash" : "none");
    lua_info_bool(w, "console_only", consoleonly_state() != CONSOLEONLY_NO);
}

// The same order as `uptime`: a fault record says the most.
static void reset_table(lua_info_t *w) {
    const char *fault = fault_last();
    lua_info_str(w, "cause", fault ? "fault" : glitchdet_caused_reset() ? "glitch"
                 : wdog_caused_reset() ? "watchdog" : "other");
    lua_info_str(w, "detail", fault);
    lua_info_str(w, "reason", resetwhy_name(resetwhy()));   // core/resetwhy.h: every reason
}

static void glitch_table(lua_info_t *w) {
    unsigned setting = config_glitch_mode(settings()->glitch_detector);
    lua_info_str(w, "mode", setting == 0 ? "off" : setting >= 5 ? "watch" : "on");
    if (setting)
        lua_info_int(w, "sensitivity", (setting - 1) & 3u);
    lua_info_bool(w, "armed", glitchdet_armed_sensitivity() >= 0);
    lua_info_bool(w, "guard_tripped", glitchdet_guard_tripped());
    lua_info_int(w, "resets", glitchdet_resets());
    lua_info_bool(w, "caused_reset", glitchdet_caused_reset());
    if (glitchdet_caused_reset()) {
        lua_info_open(w, "last");
        lua_info_int(w, "detectors", glitchdet_last_triggers());
        lua_info_int(w, "uptime", glitchdet_last_uptime_s());
        temp(w, "temp", glitchdet_last_temp());
        volts(w, "vsys", glitchdet_last_vsys());
        lua_info_close(w);
    }
    if (setting >= 5 && glitchdet_watch_triggers()) {
        lua_info_open(w, "watched");
        lua_info_int(w, "detectors", glitchdet_watch_triggers());
        lua_info_int(w, "uptime", glitchdet_watch_first_s());
        temp(w, "temp", glitchdet_watch_first_temp());
        volts(w, "vsys", glitchdet_watch_first_vsys());
        lua_info_close(w);
    }
}

static void wifi_table(lua_info_t *w) {
    lua_info_str(w, "state", wifi_state());
    lua_info_bool(w, "up", wifi_up());
    lua_info_str(w, "ssid", wifi_ssid());
    wifi_radio_t r;
    if (wifi_radio(&r)) {
        char bssid[18];
        snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x", r.bssid[0], r.bssid[1],
                 r.bssid[2], r.bssid[3], r.bssid[4], r.bssid[5]);
        lua_info_int(w, "rssi", r.rssi);
        lua_info_int(w, "channel", r.channel);
        lua_info_str(w, "bssid", bssid);
        if (r.noise)
            lua_info_int(w, "noise", r.noise);
        if (r.rate)
            lua_info_num(w, "rate_mbps", r.rate / 2.0);
        if (r.txpower && r.txpower != 127)
            lua_info_num(w, "txpower_dbm", r.txpower / 4.0);
    }
    if (wifi_up()) {
        char ip[16], gw[16];
        cyw43_arch_lwip_begin();
        struct netif *n = &cyw43_state.netif[CYW43_ITF_STA];
        ip4addr_ntoa_r(netif_ip4_addr(n), ip, sizeof(ip));
        ip4addr_ntoa_r(netif_ip4_gw(n), gw, sizeof(gw));
        cyw43_arch_lwip_end();
        lua_info_str(w, "ip", ip);
        lua_info_str(w, "gateway", gw);
    }
    unsigned arp_in, arp_out;
    wifi_arp_counts(&arp_in, &arp_out);
    lua_info_int(w, "arp_received", arp_in);
    lua_info_int(w, "arp_sent", arp_out);
    // What the `wifi` check judges, since boot (docs/design/health.md §9.9).
    wifi_counts_t wc;
    wifi_counts(&wc);
    char fw[16];
    if (countcheck_fw_version(wc.ver, fw, sizeof(fw)))
        lua_info_str(w, "firmware", fw);
    lua_info_int(w, "drops", wc.drops);
    wifistat_t ws;
    wifistat_get(&ws);
    lua_info_open(w, "errors");
    lua_info_int(w, "timeout", ws.timeout);
    lua_info_int(w, "io", ws.io);
    lua_info_int(w, "bus", ws.bus);
    lua_info_int(w, "tx", ws.tx);
    lua_info_int(w, "other", ws.other);
    lua_info_close(w);
}

// What `time` shows. The RTC is read (an I2C transfer of a few bytes).
static void clock_table(lua_info_t *w) {
    timesync_info_t i;
    timesync_info(&i);
    lua_info_bool(w, "set", i.source != NULL);
    lua_info_str(w, "source", i.source);
    if (i.source) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        lua_info_int(w, "time", tv.tv_sec);
    }
    lua_info_str(w, "server", i.server);
    lua_info_bool(w, "server_default", i.server_default);
    lua_info_int(w, "auto_hours", i.auto_hours);
    if (i.syncing) {
        lua_info_str(w, "syncing", i.syncing);
        lua_info_bool(w, "syncing_sntp", i.syncing_sntp);
    }
    if (i.synced_ago_s != TIMESYNC_NEVER) {
        lua_info_open(w, "last_sync");
        lua_info_int(w, "ago", i.synced_ago_s);
        if (!i.step_set)
            lua_info_int(w, "step_ms", i.step_ms);
        if (i.source && !strcmp(i.source, "nts"))
            lua_info_int(w, "delay_ms", i.delay_ms);
        lua_info_close(w);
    }
    if (i.failed_ago_s != TIMESYNC_NEVER) {
        lua_info_open(w, "last_failure");
        lua_info_int(w, "ago", i.failed_ago_s);
        lua_info_str(w, "why", i.failed_why);
        if (i.retry_in_s)
            lua_info_int(w, "retry_in", i.retry_in_s);
        lua_info_close(w);
    }
    lua_info_open(w, "rtc");
    int64_t t;
    rtc_status_t st = rtc_read(&t);
    lua_info_str(w, "status", rtc_status_str(st));
    if (st == RTC_OK) {
        lua_info_int(w, "time", t);
        lua_info_bool(w, "battery_low", rtc_battery_low());
    }
    char cal[40];
    timesync_cal_str(cal, sizeof(cal));
    lua_info_str(w, "cal", cal);
    lua_info_close(w);
}

// What `who` lists: one entry per shell, in slot order.
static void who_table(lua_info_t *w) {
    chat_peer_t p[CHAT_MAX_PEERS];
    unsigned n = chat_peers(p, CHAT_MAX_PEERS);
    for (unsigned i = 0; i < n; i++) {
        lua_info_open(w, "");
        lua_info_int(w, "slot", p[i].slot);
        lua_info_str(w, "name", p[i].name);
        lua_info_str(w, "from", p[i].from);
        lua_info_int(w, "seconds", p[i].since_s);
        lua_info_bool(w, "chat", p[i].in_chat);
        lua_info_close(w);
    }
}

// What `ssh` shows. The sessions are a list: login, address, state, sftp, seconds.
static void ssh_table(lua_info_t *w) {
    sshd_info_t s;
    lua_info_bool(w, "running", sshd_info(&s));
    if (!s.fingerprint)
        return;
    lua_info_str(w, "key_type", s.key_type);
    lua_info_str(w, "fingerprint", s.fingerprint);
    lua_info_int(w, "max_sessions", s.max_sessions);
    lua_info_int(w, "logins", s.logins);
    lua_info_int(w, "refused_busy", s.refused_busy);
    lua_info_int(w, "refused_penalty", s.refused_penalty);
    lua_info_int(w, "dropped_stale", s.dropped_stale);
    lua_info_open(w, "sessions");
    for (unsigned i = 0; i < s.max_sessions; i++) {
        sshd_session_info_t ss;
        if (!sshd_session(i, &ss))
            continue;
        lua_info_open(w, "");
        lua_info_int(w, "slot", i + 1);
        lua_info_str(w, "login", ss.login);
        lua_info_str(w, "address", ss.address);
        lua_info_str(w, "state", ss.state);
        lua_info_bool(w, "sftp", ss.sftp);
        lua_info_int(w, "seconds", ss.seconds);
        lua_info_close(w);
    }
    lua_info_close(w);
}

// What `tls roots` shows: the built-in roots by name, and the card's file. Reading and checking
// the file takes what `tls roots` takes (up to CERTSTORE_FILE_MAX from the card, hashed).
static void tls_table(lua_info_t *w) {
    lua_info_open(w, "builtin");
    for (unsigned i = 0; i < tls_root_count; i++)
        lua_info_str(w, "", tls_roots[i].name);
    lua_info_close(w);
    certstore_info_t c;
    certstore_info(&c);
    lua_info_open(w, "file");
    lua_info_str(w, "path", CERTSTORE_FILE);
    lua_info_str(w, "state", c.state);
    lua_info_bool(w, "used", c.used);
    if (c.used)
        lua_info_int(w, "count", c.count);
    lua_info_str(w, "why", c.why);
    lua_info_close(w);
}

// What `sd info` shows. Counting free space can take a moment on a FAT32 card (exFAT keeps a
// bitmap); `sd info` does the same from any shell.
static void sd_table(lua_info_t *w) {
    sdcard_info_t i;
    lua_info_bool(w, "mounted", sdcard_info(&i));
    // What the `sd-bus` check judges: since the card's last initialisation (§9.9). From RAM.
    sdcard_counters_t c;
    sdcard_counters(&c);
    if (c.ever_ok) {
        const sdcard_ctr_t *e = &c.since_init;
        lua_info_open(w, "errors");
        lua_info_int(w, "failed_reads", e->failed_reads);
        lua_info_int(w, "failed_writes", e->failed_writes);
        lua_info_int(w, "crc", e->crc);
        lua_info_int(w, "cmd", e->cmd);
        lua_info_int(w, "write", e->write);
        lua_info_int(w, "timeouts", e->timeout);
        lua_info_close(w);
        lua_info_int(w, "spi_khz", c.khz);
    }
    if (!i.card)
        return;
    lua_info_str(w, "card", i.card);
    lua_info_int(w, "card_bytes", (int64_t)i.card_bytes);
    lua_info_bool(w, "encrypted", i.encrypted);
    lua_info_str(w, "fs", i.fs);
    if (i.free_ok) {
        lua_info_int(w, "total", (int64_t)i.total);
        lua_info_int(w, "free", (int64_t)i.free);
        lua_info_int(w, "cluster", i.cluster);
    }
}

// Room kept after the entries for the list's end and the three fields after it.
#define DMESG_TAIL 64

// One page of the log after `cursor`: {entries = { {time=, level=, facility=, text=}, ... },
// cursor=, more=, lost=}. As many entries as fit; `more` says there are others after them, which
// the next page (from the returned cursor) gives. `lost`: entries after the given cursor were
// overwritten (the history keeps only LOG_HISTORY_SIZE bytes).
static void dmesg_table(lua_info_t *w, uint32_t cursor) {
    bool lost = false, more = false, first = true;
    log_entry_t e;
    lua_info_open(w, "entries");
    for (;;) {
        uint32_t at = cursor;
        bool jumped;
        if (!log_history_entry(&cursor, &e, &jumped))
            break;
        if (first)
            lost = jumped;
        first = false;
        lua_info_t saved = *w;
        int level = e.priority & SOS_LOG_LEVEL_MASK;
        lua_info_open(w, "");
        lua_info_num(w, "time", (double)e.uptime_ms / 1000.0);
        lua_info_str(w, "level", log_level_name(level < SOS_LOG_ERR ? SOS_LOG_ERR : level));
        const char *facility = log_facility_name(e.priority & SOS_LOG_FACILITY_MASK);
        lua_info_str(w, "facility", facility ? facility : "none");
        lua_info_str(w, "text", e.text);
        lua_info_close(w);
        if (w->failed || w->cap - w->len < DMESG_TAIL) {
            *w = saved;                         // the next page starts with this one
            cursor = at;                        // a position read before: not the first entry,
            more = true;                        // which always fits
            break;
        }
    }
    lua_info_close(w);
    lua_info_int(w, "cursor", cursor);
    lua_info_bool(w, "more", more);
    lua_info_bool(w, "lost", lost);
}

size_t lua_health_fill(lua_health_table_t which, uint32_t arg, uint8_t *buf, size_t cap) {
    lua_info_t w;
    lua_info_begin(&w, buf, cap);
    switch (which) {
    case LH_SENSORS: sensors_table(&w); break;
    case LH_MEM:     mem_table(&w); break;
    case LH_VERSION: version_table(&w); break;
    case LH_RESET:   reset_table(&w); break;
    case LH_GLITCH:  glitch_table(&w); break;
    case LH_WIFI:    wifi_table(&w); break;
    case LH_CLOCK:   clock_table(&w); break;
    case LH_WHO:     who_table(&w); break;
    case LH_SSH:     ssh_table(&w); break;
    case LH_TLS:     tls_table(&w); break;
    case LH_SD:      sd_table(&w); break;
    case LH_DMESG:   dmesg_table(&w, arg); break;
    case LH_HEALTH:  lua_health_table(&w); break;     // results only: no check is run
    default:         return 0;
    }
    return lua_info_end(&w);
}
