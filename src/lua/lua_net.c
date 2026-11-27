// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// sos.wifi.scan, sos.net.resolve and sos.ping on core 0: see lua_net.h.

#include "lua/lua_net.h"

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"

#include "lua/lua_bridge.h"
#include "lua/lua_info.h"
#include "net/ping.h"
#include "net/wifi.h"

#define SCAN_TIMEOUT_US 5000000         // a scan takes about 0.5 s
#define DNS_TIMEOUT_US  20000000        // as ping's backstop: lwIP's own retries end sooner

static int pending = -1;                // the request being served, or -1
static uint64_t started_us;

static bool scan_mine;                  // the scan's name collection is ours to stop

// The lookup's answer, from lwIP's callback (under the lwIP lock). dns_id tells a late answer to
// an abandoned lookup from the current one's.
static uint32_t dns_id;
static bool dns_done, dns_ok;
static ip_addr_t dns_addr;

static ping_run_t *run;

static void dns_cb(const char *name, const ip_addr_t *addr, void *arg) {
    (void)name;
    if ((uint32_t)(uintptr_t)arg != dns_id)
        return;
    if (addr)
        dns_addr = *addr;
    dns_ok = addr != NULL;
    dns_done = true;
}

void lua_net_drop(void) {
    if (run) {
        ping_end(run);
        run = NULL;
    }
    if (scan_mine && wifi_names_active())
        wifi_names_stop();
    scan_mine = false;
    cyw43_arch_lwip_begin();
    dns_id++;                               // a late answer is ignored
    cyw43_arch_lwip_end();
    pending = -1;
}

// Start the request; false with *err if it can't.
static bool start(int type, const char *host, uint32_t count, const char **err) {
    pending = type;
    started_us = time_us_64();
    switch (type) {
    case LR_SCAN:
        scan_mine = !wifi_names_active();
        wifi_names_start();
        if (!wifi_names_active()) {
            *err = "Wi-Fi isn't running";
            return false;
        }
        return true;
    case LR_RESOLVE: {
        if (!wifi_up()) {
            *err = "Wi-Fi is not connected";
            return false;
        }
        cyw43_arch_lwip_begin();
        uint32_t id = ++dns_id;
        dns_done = dns_ok = false;
        err_t e = dns_gethostbyname(host, &dns_addr, dns_cb, (void *)(uintptr_t)id);
        if (e == ERR_OK)
            dns_done = dns_ok = true;           // an address, or a name in lwIP's cache
        else if (e != ERR_INPROGRESS)
            dns_done = true;
        cyw43_arch_lwip_end();
        return true;
    }
    case LR_PING:
        run = ping_begin(host, count, err);
        return run != NULL;
    default:
        *err = "unknown request";
        return false;
    }
}

static void add_network(const char *ssid, int rssi, void *arg) {
    lua_info_t *w = arg;
    lua_info_open(w, "");
    lua_info_str(w, "ssid", ssid);
    lua_info_int(w, "rssi", rssi);
    lua_info_close(w);
}

static void count_network(const char *ssid, int rssi, void *arg) {
    (void)ssid;
    (void)rssi;
    (void)arg;
}

// Is the request finished? Then its answer is in w, or *err is set.
static bool poll(lua_info_t *w, const char **err) {
    uint64_t elapsed = time_us_64() - started_us;
    switch (pending) {
    case LR_SCAN:
        if (wifi_names_each(count_network, NULL) && elapsed < SCAN_TIMEOUT_US)
            return false;                       // still scanning
        wifi_names_each(add_network, w);        // strongest first, as Tab lists them
        return true;
    case LR_RESOLVE: {
        bool done, ok;
        ip_addr_t addr;
        cyw43_arch_lwip_begin();
        done = dns_done;
        ok = dns_ok;
        addr = dns_addr;
        cyw43_arch_lwip_end();
        if (!done && elapsed < DNS_TIMEOUT_US)
            return false;
        if (!done || !ok) {
            *err = done ? "DNS lookup failed" : "DNS lookup timed out";
            return true;
        }
        char ip[IPADDR_STRLEN_MAX];
        ipaddr_ntoa_r(&addr, ip, sizeof(ip));
        lua_info_str(w, "address", ip);
        return true;
    }
    case LR_PING: {
        if (ping_advance(run))
            return false;
        if (ping_failure(run)) {
            *err = ping_failure(run);
            return true;
        }
        const ping_stats_t *s = ping_run_stats(run);
        char ip[IPADDR_STRLEN_MAX];
        ping_run_address(run, ip, sizeof(ip));
        lua_info_str(w, "address", ip);
        lua_info_int(w, "sent", s->sent);
        lua_info_int(w, "received", s->received);
        lua_info_int(w, "errors", s->icmp_errors);
        lua_info_int(w, "not_sent", s->not_sent);
        if (s->received) {
            lua_info_num(w, "min_ms", s->min_us / 1000.0);
            lua_info_num(w, "avg_ms", (double)s->sum_us / s->received / 1000.0);
            lua_info_num(w, "max_ms", s->max_us / 1000.0);
        }
        return true;
    }
    default:
        *err = "unknown request";
        return true;
    }
}

bool lua_net_serve(int type, const char *host, uint32_t count, uint8_t *out, size_t cap,
                   size_t *len, const char **err) {
    *err = NULL;
    *len = 0;
    if (pending != type) {
        lua_net_drop();
        if (!start(type, host, count, err)) {
            lua_net_drop();
            return true;
        }
    }
    lua_info_t w;
    lua_info_begin(&w, out, cap);
    if (!poll(&w, err))
        return false;
    if (!*err) {
        *len = lua_info_end(&w);
        if (!*len && w.failed)
            *err = "the answer didn't fit";
    }
    lua_net_drop();
    return true;
}
