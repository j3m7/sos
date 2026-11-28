// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

// The `wifi` and `net` commands (moved from shell/shell.c, docs/design/build-features.md §4.1).

#include "net/wifi_cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "lwip/memp.h"
#include "lwip/netif.h"
#include "lwip/stats.h"

#include "core/config.h"
#include "core/countcheck.h"
#include "core/heaptag.h"
#include "core/log.h"
#include "core/settings.h"
#include "net/wifi.h"
#include "net/wifistat.h"

static void cmd_net(shell_t *sh) {
    int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    const char *s = link == CYW43_LINK_UP ? "up" : link == CYW43_LINK_JOIN ? "joined"
                  : link == CYW43_LINK_NOIP ? "no ip" : link == CYW43_LINK_FAIL ? "failed"
                  : link == CYW43_LINK_NONET ? "no network" : link == CYW43_LINK_BADAUTH ? "bad auth"
                  : "down";
    int32_t rssi = 0;
    cyw43_wifi_get_rssi(&cyw43_state, &rssi);

    char ip[16], gw[16];
    const char *host;
    cyw43_arch_lwip_begin();
    struct netif *n = &cyw43_state.netif[CYW43_ITF_STA];
    ip4addr_ntoa_r(netif_ip4_addr(n), ip, sizeof(ip));
    ip4addr_ntoa_r(netif_ip4_gw(n), gw, sizeof(gw));
    host = netif_get_hostname(n);
    cyw43_arch_lwip_end();

    shell_printf(sh, "wifi:  %s, rssi %ld dBm\n", s, (long)rssi);
    shell_printf(sh, "ip:    %s\n", ip);
    shell_printf(sh, "gw:    %s\n", gw);
    shell_printf(sh, "host:  %s\n", host ? host : "");
    unsigned arp_in, arp_out;
    wifi_arp_counts(&arp_in, &arp_out);
    shell_printf(sh, "arp:   %u received, %u sent; keepalive %s\n", arp_in, arp_out,
                 wifi_keepalive() ? "on" : "off (testing)");
}

void net_command(shell_t *sh, char *args) { (void)args; cmd_net(sh); }

// --- Wi-Fi ---------------------------------------------------------------------------------------

static char pending_ssid[CONFIG_SSID_MAX + 1];

// The Wi-Fi settings and state, for `wifi` and `config`. "wifi password" says whether one is
// saved (never the password itself); the board has no login password, SSH is keys only.
void wifi_show(shell_t *sh) {
    const sos_config_t *c = settings();
    unsigned n = config_wifi_count(c);
    const char *now = wifi_ssid();
    if (!n)
        shell_printf(sh, "wifi network:  (not configured)\n");
    for (unsigned i = 0; i < n; i++) {
        config_wifi_net_t net;
        config_wifi_get(c, i, &net);
        shell_printf(sh, "wifi network %u: %s, %s%s\n", i + 1, net.ssid,
                     net.password[0] ? "password set" : "open network",
                     now && !strcmp(now, net.ssid) ? " (connected)" : "");
        memset(&net, 0, sizeof(net));
    }
    shell_printf(sh, "wifi state:    %s\n", wifi_state());
    shell_printf(sh, "wifi power:    saving %s\n", c->wifi_powersave ? "on" : "off");
    shell_printf(sh, "wifi country:  %s\n", c->wifi_country[0] ? c->wifi_country : "worldwide");
}

static void stats_row(shell_t *sh, const char *name, const struct stats_proto *p) {
    shell_printf(sh, "  %-5s sent %-8u received %-8u dropped %u (checksum %u, length %u, memory %u, "
                     "protocol %u, other %u)\n", name, (unsigned)p->xmit, (unsigned)p->recv,
                 (unsigned)p->drop, (unsigned)p->chkerr, (unsigned)p->lenerr, (unsigned)p->memerr,
                 (unsigned)p->proterr, (unsigned)p->err);
}

// Radio state and packet counters, for chasing packet loss.
static void wifi_stats(shell_t *sh, bool chip) {
    wifi_radio_t r;
    if (wifi_radio(&r))
        shell_printf(sh, "radio:  signal %d dBm, channel %u, access point "
                         "%02x:%02x:%02x:%02x:%02x:%02x\n", (int)r.rssi, (unsigned)r.channel,
                     r.bssid[0], r.bssid[1], r.bssid[2], r.bssid[3], r.bssid[4], r.bssid[5]);
    else
        shell_printf(sh, "radio:  not connected\n");
    if (r.channel)
        shell_printf(sh, "        noise %d dBm, transmit rate %d.%d Mbit/s, transmit power limit %s"
                         "%d.%02d dBm\n", (int)r.noise, (int)(r.rate / 2), (int)(r.rate % 2) * 5,
                     r.txpower == 127 ? "not set: " : "", (int)(r.txpower / 4),
                     (int)(r.txpower % 4) * 25);
    struct stats_ st;
    cyw43_arch_lwip_begin();
    st = lwip_stats;
    struct stats_mem pool = *lwip_stats.memp[MEMP_PBUF_POOL], seg = *lwip_stats.memp[MEMP_TCP_SEG],
                     pcb = *lwip_stats.memp[MEMP_TCP_PCB];
    cyw43_arch_lwip_end();
    shell_printf(sh, "packets since boot:\n");
    stats_row(sh, "arp", &st.etharp);
    stats_row(sh, "ip", &st.ip);
    stats_row(sh, "icmp", &st.icmp);
    stats_row(sh, "udp", &st.udp);
    stats_row(sh, "tcp", &st.tcp);
    shell_printf(sh, "tcp:    %u segments resent, %u connections reset or failed\n",
                 (unsigned)st.mib2.tcpretranssegs,
                 (unsigned)(st.mib2.tcpattemptfails + st.mib2.tcpestabresets));
    shell_printf(sh, "pools:  packets %u/%u in use (most %u, %u failures); segments most %u/%u "
                     "(%u failures); connections most %u/%u (%u failures); heap most %u/%u "
                     "(%u failures)\n",
                 (unsigned)pool.used, (unsigned)pool.avail, (unsigned)pool.max, (unsigned)pool.err,
                 (unsigned)seg.max, (unsigned)seg.avail, (unsigned)seg.err, (unsigned)pcb.max,
                 (unsigned)pcb.avail, (unsigned)pcb.err, (unsigned)st.mem.max,
                 (unsigned)st.mem.avail, (unsigned)st.mem.err);
    // The driver's errors and the link's drops (docs/design/health.md §9.9: the `wifi` check).
    wifistat_t ws;
    wifistat_get(&ws);
    wifi_counts_t wc;
    wifi_counts(&wc);
    char fw[16];
    countcheck_fw_version(wc.ver, fw, sizeof(fw));
    shell_printf(sh, "driver: %u timeouts, %u bus, %u io, %u tx, %u other since boot; %u link "
                     "drops; firmware %s\n", (unsigned)ws.timeout, (unsigned)ws.bus,
                 (unsigned)ws.io, (unsigned)ws.tx, (unsigned)ws.other, (unsigned)wc.drops,
                 fw[0] ? fw : "unknown");
    if (!chip)
        return;
    // The chip's counters come as a version/length word and 32-bit values. Their layout differs
    // between firmware versions and isn't published for this one, so they're shown raw; compare
    // two readings. Positions in version 10, worked out from how they move (not confirmed):
    // 0 frames sent, 2 resent, 15 received, 45 beacon times, 49 RTS sent, 75 CTS received,
    // 85 our access point's beacons received, 87 other networks' beacons, 88 response timeouts,
    // 67/68 received with a bad checksum / header, 70 receptions started.
    // The buffer the chip answers into (1 KB), for this command only. The first 128 words are
    // shown, as before.
    uint32_t *c = heap_alloc(HEAP_NET, WIFI_CHIP_COUNTER_WORDS * sizeof(uint32_t));
    unsigned n = c ? wifi_chip_counters(c) : 0;
    if (n > 128)
        n = 128;
    if (!n) {
        shell_printf(sh, "chip counters: %s\n", c ? "not available" : "out of memory");
        heap_release(HEAP_NET, c);
        return;
    }
    shell_printf(sh, "chip counters (version %u, %u bytes), index: value, zeros omitted:\n",
                 (unsigned)(c[0] & 0xffff), (unsigned)(c[0] >> 16));
    for (unsigned i = 1, col = 0; i < n; i++)
        if (c[i])
            shell_printf(sh, "%3u: %-10u%s", i - 1, (unsigned)c[i], ++col % 5 ? " " : "\n");
    shell_printf(sh, "\n");
    heap_release(HEAP_NET, c);
}

// `wifi set` (arg NULL) replaces the first network; `wifi add` (arg non-NULL) adds one after
// the others.
static void on_wifi_password(shell_t *sh, char *answer, void *arg) {
    if (!answer)
        return;
    sos_config_t *c = settings();
    bool add = arg != NULL;
    const char *err = add ? config_wifi_add(c, pending_ssid, answer)
                          : config_wifi_set_first(c, pending_ssid, answer);
    memset(answer, 0, strlen(answer));
    if (err) {
        shell_printf(sh, "not changed: %s\n", err);
        return;
    }
    shell_save_settings(sh);
    if (add) {
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: Wi-Fi network '%s' added", pending_ssid);
        if (!wifi_up())
            wifi_reconnect();       // it may be the one in reach
    } else {
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: Wi-Fi network set to '%s'", pending_ssid);
        wifi_reconnect();
    }
}

// Tab at an SSID question: the networks a scan hears (net/wifi.c), strongest first, each with
// its signal in the list.
typedef struct {
    shell_value_fn fn;
    void *arg;
} ssid_tab_t;

static void ssid_one(const char *ssid, int rssi, void *arg) {
    ssid_tab_t *t = arg;
    char note[16];
    snprintf(note, sizeof(note), "%d dBm", rssi);
    t->fn(ssid, note, t->arg);
}

static bool tab_ssids(shell_t *sh, shell_value_fn fn, void *arg) {
    (void)sh;
    ssid_tab_t t = {fn, arg};
    return wifi_names_each(ssid_one, &t);
}

// Ask for an SSID, with Tab offering the networks around: a scan starts now (about 0.5 s). The
// answer's callback calls wifi_names_stop().
void wifi_ask_ssid(shell_t *sh, const char *prompt, shell_answer_fn fn, void *arg) {
    shell_ask(sh, prompt, false, fn, arg);
    wifi_names_start();
    shell_ask_complete(sh, tab_ssids);
}

static void on_wifi_ssid(shell_t *sh, char *answer, void *arg) {
    wifi_names_stop();
    if (!answer)
        return;
    const char *err = config_check_ssid(answer);
    if (!err && arg && config_wifi_find(settings(), answer) >= 0)
        err = "that network is already saved (remove it first to change its password)";
    if (!err && arg && config_wifi_count(settings()) == CONFIG_WIFI_NETS)
        err = "the network list is full (5 networks)";
    if (err) {
        shell_printf(sh, "not changed: %s\n", err);
        return;
    }
    strcpy(pending_ssid, answer);
    shell_ask(sh, "Password (empty for an open network): ", true, on_wifi_password, arg);
}

// `wifi remove <n>` and `wifi prefer <n>`: n as `wifi` numbers them. Returns the index, or -1
// after saying why not.
static int wifi_number(shell_t *sh, const char *arg) {
    char *end;
    long n = strtol(arg, &end, 10);
    if (*arg == '\0' || *end != '\0' || n < 1 || n > (long)config_wifi_count(settings())) {
        shell_printf(sh, "no network %s ('wifi' lists them)\n", arg);
        return -1;
    }
    return (int)n - 1;
}

void wifi_command(shell_t *sh, char *args) {
    if (!args || !*args) {
        wifi_show(sh);
    } else if (!strcmp(args, "set")) {
        if (shell_require_admin(sh))
            wifi_ask_ssid(sh, "SSID: ", on_wifi_ssid, NULL);
    } else if (!strcmp(args, "add")) {
        if (shell_require_admin(sh))
            wifi_ask_ssid(sh, "SSID: ", on_wifi_ssid, (void *)1);
    } else if (!strncmp(args, "remove ", 7)) {
        int i;
        if (!shell_require_admin(sh) || (i = wifi_number(sh, args + 7)) < 0)
            return;
        config_wifi_net_t net;
        config_wifi_get(settings(), (unsigned)i, &net);
        const char *now = wifi_ssid();
        bool was_connected = now && !strcmp(now, net.ssid);
        config_wifi_remove(settings(), (unsigned)i);
        shell_save_settings(sh);
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: Wi-Fi network '%s' removed", net.ssid);
        memset(&net, 0, sizeof(net));
        if (!config_wifi_count(settings()))
            shell_printf(sh, "warning: no Wi-Fi network left\n");
        if (was_connected)
            wifi_reconnect();
    } else if (!strncmp(args, "prefer ", 7)) {
        int i;
        if (!shell_require_admin(sh) || (i = wifi_number(sh, args + 7)) < 0)
            return;
        config_wifi_prefer(settings(), (unsigned)i);
        shell_save_settings(sh);
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE,
                "config: Wi-Fi network '%s' preferred", settings()->wifi_ssid);
        if (wifi_up() && strcmp(wifi_ssid(), settings()->wifi_ssid))
            shell_printf(sh, "takes effect on the next Wi-Fi connect ('wifi connect')\n");
    } else if (!strcmp(args, "connect")) {
        if (shell_require_admin(sh))
            wifi_reconnect();
    } else if (!strcmp(args, "keepalive on") || !strcmp(args, "keepalive off")) {
        // For testing only, so not saved: it's back on after a reboot.
        if (!shell_require_admin(sh))
            return;
        wifi_set_keepalive(!strcmp(args, "keepalive on"));
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE,
                "wifi: keepalive %s (until reboot)", wifi_keepalive() ? "on" : "off");
    } else if (!strcmp(args, "stats") || !strcmp(args, "stats chip")) {
        wifi_stats(sh, args[5] != 0);
#ifdef SOS_TEST_COMMANDS
    } else if (!strcmp(args, "fault drop")) {
        // Test builds (docs/design/health.md §9.9): a link drop nobody asked for, as the `wifi`
        // check counts one; wifi.c sees the link go and reconnects.
        if (!shell_require_admin(sh))
            return;
        if (wifi_fault_drop()) {
            log_msg(SOS_LOG_DAEMON | SOS_LOG_NOTICE, "wifi: fault: link dropped (test command)");
            shell_printf(sh, "dropped: wifi reconnects as after a real drop\n");
        } else {
            shell_printf(sh, "not connected\n");
        }
#endif
    } else if (!strncmp(args, "iovar ", 6)) {
        // Debugging only: `wifi iovar <name> [value]` reads or writes a chip variable.
        if (!shell_require_admin(sh))
            return;
        char *name = args + 6, *val = strchr(name, ' ');
        if (val)
            *val++ = '\0';
        uint32_t v = val ? (uint32_t)strtoul(val, NULL, 0) : 0;
        int r = val ? wifi_iovar_set(name, v) : wifi_iovar_get(name, &v);
        shell_printf(sh, "%s %s: result %d, value %u (0x%x)\n", val ? "set" : "get", name, r,
                     (unsigned)v, (unsigned)v);
    } else if (!strncmp(args, "country ", 8)) {
        if (!shell_require_admin(sh))
            return;
        const char *v = !strcmp(args + 8, "worldwide") ? "" : args + 8;
        const char *err = config_check_country(v);
        if (err) {
            shell_printf(sh, "not changed: %s\n", err);
            return;
        }
        snprintf(settings()->wifi_country, sizeof(settings()->wifi_country), "%s", v);
        shell_save_settings(sh);
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: wifi country %s", v[0] ? v : "worldwide");
        shell_printf(sh, "takes effect at the next boot ('reboot')\n");
    } else if (!strcmp(args, "power")) {
        shell_printf(sh, "power saving: %s\n", settings()->wifi_powersave ? "on" : "off");
    } else if (!strcmp(args, "power on") || !strcmp(args, "power off")) {
        if (!shell_require_admin(sh))
            return;
        settings()->wifi_powersave = !strcmp(args, "power on");
        shell_save_settings(sh);
        wifi_apply_power();
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE,
                "config: wifi power saving %s", settings()->wifi_powersave ? "on" : "off");
    } else {
        shell_printf(sh, "usage: wifi [set|add|remove <n>|prefer <n>|connect|stats [chip]|power [on|off]|\n            country <XX>|worldwide|keepalive on|off|iovar <name> [<value>]]\n");
    }
}

// `wifi remove|prefer <n>`: the saved networks' numbers, as `wifi` shows them, with their SSIDs.
static void tab_wifi_numbers(shell_t *sh, shell_value_fn fn, void *arg) {
    (void)sh;
    char n[4];
    config_wifi_net_t net;
    for (unsigned i = 0; config_wifi_get(settings(), i, &net); i++) {
        snprintf(n, sizeof(n), "%u", i + 1);
        fn(n, net.ssid, arg);
    }
}

static const shell_values_t wifi_numbers = SHELL_VALUES_INIT(tab_wifi_numbers);

// The words Tab offers (shell.h).
#define END {0}
#define W(name) {name, NULL, 0}
static const shell_word_t on_off[] = {W("on"), W("off"), END};
static const shell_word_t on_off_admin[] = {
    {"on", NULL, SHELL_ADMIN}, {"off", NULL, SHELL_ADMIN}, END};
const shell_word_t wifi_words[] = {
    {"stats", (const shell_word_t[]){W("chip"), END}, 0},
    {"set", NULL, SHELL_ADMIN},
    {"add", NULL, SHELL_ADMIN},
    {"remove", SHELL_VALUES_OF(wifi_numbers), SHELL_ADMIN},
    {"prefer", SHELL_VALUES_OF(wifi_numbers), SHELL_ADMIN},
    {"connect", NULL, SHELL_ADMIN},
    {"power", on_off_admin, 0},             // shows the setting anywhere; on|off: console
    {"country", (const shell_word_t[]){W("worldwide"), END}, SHELL_ADMIN},
    {"keepalive", on_off, SHELL_ADMIN},
    {"iovar", NULL, SHELL_ADMIN},
#ifdef SOS_TEST_COMMANDS
    {"fault", (const shell_word_t[]){W("drop"), END}, SHELL_ADMIN | SHELL_HIDDEN},
#endif
    END};
#undef W
#undef END
