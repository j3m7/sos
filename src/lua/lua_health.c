// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Core 0's answers to LR_INFO: the read-only tables of sos.sensors, sos.mem, sos.version,
// sos.reset_reason, sos.glitch, sos.wifi.status and sos.dmesg, written with lua_info.h. Each
// reads what the matching shell command (temp and power, mem, version, uptime, glitch, net and
// wifi, dmesg) shows, and all of it may be shown over SSH. None of it waits: the sensors' latest readings are used (taken
// every SENSORS_SAMPLE_S by the main loop), not a new reading.
//
// A field the board doesn't know is left out, so the script sees nil: the temp() and volts()
// helpers skip SENSORS_NONE, and lua_info_str() writes nothing for a NULL string (wifi_ssid()
// when not connected, fault_last() when there was no fault).

#include "lua/lua_health.h"

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "lwip/netif.h"
#include "lwip/stats.h"

#include "core/chipinfo.h"
#include "core/config.h"
#include "core/consoleonly.h"
#include "core/fault.h"
#include "core/glitchdet.h"
#include "core/heap.h"
#include "core/heaptag.h"
#include "core/log.h"
#include "core/otpread.h"
#include "core/rootsource.h"
#include "core/sensors.h"
#include "core/settings.h"
#include "core/stackmon.h"
#include "core/version.h"
#include "core/wdog.h"
#include "lua/lua_core1.h"
#include "net/wifi.h"

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
    cyw43_arch_lwip_end();
    used_peak_total(w, "lwip", net.used, net.max, net.avail);
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
    case LH_DMESG:   dmesg_table(&w, arg); break;
    default:         return 0;
    }
    return lua_info_end(&w);
}
