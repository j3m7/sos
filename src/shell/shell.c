// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "shell/shell.h"
#include "shell/login.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "lwip/memp.h"
#include "lwip/netif.h"
#include "lwip/stats.h"

#include "chat/chat.h"
#include "core/chipinfo.h"
#include "core/config.h"
#include "core/fault.h"
#include "core/heap.h"
#include "core/log.h"
#include "core/rootflash.h"
#include "core/rootsecret.h"
#include "core/settings.h"
#include "core/stackmon.h"
#include "core/version.h"
#include "core/wdog.h"
#include "edit/edit.h"
#include "files/files.h"
#include "files/sdcard.h"
#include "lua/lua_core1.h"
#include "lua/lua_host.h"
#include "net/http.h"
#include "net/wifi.h"
#include "ssh/sshd.h"
#include "time/nts.h"
#include "time/timeconv.h"
#include "time/timesync.h"
#include "tls/certstore.h"

// --- helpers -------------------------------------------------------------------------------------

// Configuration changes need physical access: only the console channel is an admin channel.
// (So the multi-step flows below, which keep their state in statics, only ever run there.)
static bool require_admin(shell_t *sh) {
    if (sh->io->admin)
        return true;
    shell_printf(sh, "configuration can only be changed on the serial console\n");
    return false;
}

static void save(shell_t *sh) {
    if (!settings_save())
        shell_printf(sh, "error: saving to flash failed; the change will be lost at reboot\n");
}

// --- information ---------------------------------------------------------------------------------

// The commit, then which board and chip this is. The serial is the one the board shows over USB
// (and to picotool); the chip revision decides which published attacks apply (issue #64).
static void cmd_version(shell_t *sh) {
    char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1], chip[64];
    pico_get_unique_board_id_string(serial, sizeof(serial));
    chipinfo_format(chip, sizeof(chip), rp2350_chip_version(), rp2350_rom_version());
    shell_printf(sh, "SOS %s\n", sos_version);
    shell_printf(sh, "board: serial %s\n", serial);
    shell_printf(sh, "chip: %s\n", chip);
}

static void cmd_help(shell_t *sh) {
    shell_printf(sh, "help              this list\n");
    shell_printf(sh, "uptime            time since boot\n");
    shell_printf(sh, "version           firmware commit, board serial and chip revision\n");
    shell_printf(sh, "time [sync]       date and time (UTC) / set clock and RTC from NTS\n");
    shell_printf(sh, "time auto|server  periodic sync / NTS server settings\n");
    shell_printf(sh, "time cal [ppm]    RTC calibration: how fast the RTC runs\n");
    shell_printf(sh, "nts [ke]          NTS status / key exchange (development)\n");
    shell_printf(sh, "nts query         authenticated NTS time query; reports the offset\n");
    shell_printf(sh, "tls roots         trusted root certificates (built-in and SD card)\n");
    shell_printf(sh, "mem               heap and stack usage\n");
    shell_printf(sh, "net               Wi-Fi and IP status\n");
    shell_printf(sh, "ssh               SSH server status\n");
    shell_printf(sh, "who               who is logged in, and who is in the chat\n");
    shell_printf(sh, "wall <message>    send a message to every other shell\n");
    shell_printf(sh, "chat              talk with the others in the chat room (Ctrl-D leaves)\n");
    shell_printf(sh, "dmesg             system log\n");
    shell_printf(sh, "log               log levels\n");
    shell_printf(sh, "config            show the configuration\n");
    shell_printf(sh, "rootsecret        the root secret that stored keys are derived from\n");
    shell_printf(sh, "key [list]        trusted SSH public keys\n");
    shell_printf(sh, "wifi              Wi-Fi network\n");
    shell_printf(sh, "wifi stats [chip] signal, noise, packet and pool counters [and the chip's]\n");
    shell_printf(sh, "sd [info]         SD card status (also: sd speed)\n");
    files_help(sh);
    shell_printf(sh, "edit <file>       full-screen text editor (Ctrl-S saves, Ctrl-Q quits)\n");
    shell_printf(sh, "lua <file> | lua -e <code>  run a Lua script (on core 1; Ctrl-C stops)\n");
    shell_printf(sh, "lua               interactive Lua prompt (Ctrl-D or exit leaves)\n");
    shell_printf(sh, "lua -b <file>     run a script in the background (output to the log)\n");
    shell_printf(sh, "lua ps | lua stop the running script / stop it\n");
    shell_printf(sh, "http get|head <url>, http post <url> <text>  HTTPS request (testing)\n");
    if (sh->io->admin) {
        shell_printf(sh, "setup             guided configuration\n");
        shell_printf(sh, "passwd            set or change the console password\n");
        shell_printf(sh, "log console|history <level>  set log levels (until reboot)\n");
        shell_printf(sh, "time sync sntp    one unauthenticated SNTP sync (" SOS_NTP_SERVER ")\n");
        shell_printf(sh, "nts ke <host>     key exchange with another server (development)\n");
        shell_printf(sh, "wifi set|connect  change the network / reconnect\n");
        shell_printf(sh, "wifi power on|off Wi-Fi power saving (default off)\n");
        shell_printf(sh, "wifi country <XX>|worldwide  radio rules to follow, e.g. US (after reboot)\n");
        shell_printf(sh, "wifi keepalive on|off  testing: stop announcing the address (until reboot)\n");
        shell_printf(sh, "wifi iovar <name> [<value>]  debugging: read or set a Wi-Fi chip variable\n");
        shell_printf(sh, "key add [line]    trust a public key (paste an authorized_keys line)\n");
        shell_printf(sh, "key del <n>       stop trusting key n\n");
        shell_printf(sh, "hostname <name>   set the hostname\n");
        shell_printf(sh, "time auto <h>|off sync the clock from NTS every h hours (default off)\n");
        shell_printf(sh, "time server <name>|default  set the NTS server\n");
        shell_printf(sh, "tls roots approve|forget  trust /certs/roots.pem for TLS / stop\n");
        shell_printf(sh, "config reset      erase the configuration\n");
        shell_printf(sh, "crash fault|stack|panic|hang  testing: stop the firmware that way\n");
        shell_printf(sh, "sd format [exfat|fat32] [plain]\n");
        shell_printf(sh, "                  erase and format the SD card: encrypted unless plain,\n");
        shell_printf(sh, "                  showing the card's key once, to store offline\n");
        shell_printf(sh, "sd unmount        unmount the card, to remove it\n");
        shell_printf(sh, "sd test [KB [keep]]  write/read benchmark (everything else waits)\n");
    }
    shell_printf(sh, "logout            end this session (also Ctrl-D)\n");
    shell_printf(sh, "reboot            restart the device\n");
}

static void cmd_uptime(shell_t *sh) {
    uint64_t s = to_us_since_boot(get_absolute_time()) / 1000000;
    shell_printf(sh, "up %llud %02llu:%02llu:%02llu%s\n",
                 s / 86400, (s / 3600) % 24, (s / 60) % 60, s % 60,
                 fault_last() ? "" : wdog_caused_reset() ? " (last reset: watchdog)" : "");
    if (fault_last())
        shell_printf(sh, "last reset: %s\n", fault_last());
}

static void cmd_mem(shell_t *sh) {
    heap_info_t h = heap_info();
    shell_printf(sh, "heap:  %u in use, %u peak, %u total\n",
                 (unsigned)h.used, (unsigned)h.arena, (unsigned)h.total);
    shell_printf(sh, "stack: %u peak of %u bytes\n",
                 (unsigned)stackmon_peak(), (unsigned)stackmon_size());
    if (lua_core1_stack_peak())
        shell_printf(sh, "lua:   %u peak of %u bytes of core 1 stack (last script)\n",
                     (unsigned)lua_core1_stack_peak(), (unsigned)LUA_CORE1_STACK);
}

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

// --- log -----------------------------------------------------------------------------------------

static void cmd_dmesg(shell_t *sh) {
    char line[LOG_LINE_MAX + 1];
    uint32_t cursor = 0;
    while (log_history_next(&cursor, line, sizeof(line)))
        shell_printf(sh, "%s\n", line);
}

static void cmd_log(shell_t *sh, char *args) {
    char *what = args ? strtok(args, " ") : NULL;
    char *level = what ? strtok(NULL, " ") : NULL;
    if (!what) {
        shell_printf(sh, "console: %s (printed on the serial console)\n",
                     log_level_name(log_console_level()));
        shell_printf(sh, "history: %s (kept for dmesg)\n", log_level_name(log_history_level()));
        shell_printf(sh, "levels:  err warning notice info debug\n");
        return;
    }
    int l = level ? log_level_parse(level) : -1;
    if (l < 0 || (strcmp(what, "console") && strcmp(what, "history"))) {
        shell_printf(sh, "usage: log [console|history err|warning|notice|info|debug]\n");
        return;
    }
    // The log is the record of logins and configuration changes: a session can read it
    // (dmesg) but not turn it down.
    if (!require_admin(sh))
        return;
    if (!strcmp(what, "console"))
        log_set_console_level(l);
    else
        log_set_history_level(l);
    shell_printf(sh, "%s level: %s (until reboot)\n", what, level);
}

// --- keys ----------------------------------------------------------------------------------------

static void print_key(shell_t *sh, unsigned index, const config_key_t *k) {
    char type[32], fp[64];
    sshd_fingerprint(k->blob, k->len, fp, sizeof(fp));
    shell_printf(sh, "%2u  %-20s %s  %s\n", index + 1, config_key_type(k, type, sizeof(type)), fp,
                 k->comment);
}

static void key_list(shell_t *sh) {
    const sos_config_t *c = settings();
    if (!c->key_count) {
        shell_printf(sh, "no trusted keys: SSH logins are not possible%s\n",
                     sh->io->admin ? " (add one with 'key add')" : "");
        return;
    }
    for (unsigned i = 0; i < c->key_count; i++)
        print_key(sh, i, &c->keys[i]);
}

// Parse and add one authorized_keys line. Returns true if the key was added.
static bool key_add_line(shell_t *sh, const char *line) {
    config_key_t key;
    const char *err = config_key_parse(line, &key);
    if (!err)
        err = config_key_add(settings(), &key);
    if (err) {
        shell_printf(sh, "not added: %s\n", err);
        return false;
    }
    save(sh);
    shell_printf(sh, "trusted:\n");
    print_key(sh, settings()->key_count - 1, &key);
    log_msg(SOS_LOG_NOTICE, "config: trusted key added (%s)", key.comment[0] ? key.comment : "no comment");
    return true;
}

static void on_key_line(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (answer && *answer)
        key_add_line(sh, answer);
    else if (answer)
        shell_printf(sh, "nothing added\n");
}

static void cmd_key(shell_t *sh, char *args) {
    char *sub = args ? strtok(args, " ") : NULL;
    char *rest = sub ? strtok(NULL, "") : NULL;
    if (!sub || !strcmp(sub, "list")) {
        key_list(sh);
    } else if (!strcmp(sub, "add")) {
        if (!require_admin(sh))
            return;
        if (rest && *rest)
            key_add_line(sh, rest);
        else
            shell_ask(sh, "Paste the public key (e.g. ~/.ssh/id_ed25519.pub): ", false, on_key_line, NULL);
    } else if (!strcmp(sub, "del")) {
        if (!require_admin(sh))
            return;
        char *end;
        long n = rest ? strtol(rest, &end, 10) : 0;
        if (!rest || *end || n < 1 || !config_key_remove(settings(), (unsigned)(n - 1))) {
            shell_printf(sh, "usage: key del <n>, n as shown by 'key list'\n");
            return;
        }
        save(sh);
        log_msg(SOS_LOG_NOTICE, "config: trusted key %ld removed", n);
        shell_printf(sh, "removed key %ld\n", n);
        if (!settings()->key_count)
            shell_printf(sh, "warning: no trusted keys left; SSH logins are no longer possible\n");
    } else {
        shell_printf(sh, "usage: key [list] | key add [line] | key del <n>\n");
    }
}

// --- Wi-Fi ---------------------------------------------------------------------------------------

static char pending_ssid[CONFIG_SSID_MAX + 1];

// The Wi-Fi settings and state, for `wifi` and `config`. "wifi password" says whether one is
// saved (never the password itself); the board has no login password, SSH is keys only.
static void wifi_show(shell_t *sh) {
    const sos_config_t *c = settings();
    shell_printf(sh, "wifi network:  %s\n", c->wifi_ssid[0] ? c->wifi_ssid : "(not configured)");
    shell_printf(sh, "wifi password: %s\n", c->wifi_password[0] ? "set" : "none (open network)");
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
    if (!chip)
        return;
    // The chip's counters come as a version/length word and 32-bit values. Their layout differs
    // between firmware versions and isn't published for this one, so they're shown raw; compare
    // two readings. Positions in version 10, worked out from how they move (not confirmed):
    // 0 frames sent, 2 resent, 15 received, 45 beacon times, 49 RTS sent, 75 CTS received,
    // 85 our access point's beacons received, 87 other networks' beacons, 88 response timeouts,
    // 67/68 received with a bad checksum / header, 70 receptions started.
    static uint32_t c[128];
    unsigned n = wifi_chip_counters(c, 128);
    if (!n) {
        shell_printf(sh, "chip counters: not available\n");
        return;
    }
    shell_printf(sh, "chip counters (version %u, %u bytes), index: value, zeros omitted:\n",
                 (unsigned)(c[0] & 0xffff), (unsigned)(c[0] >> 16));
    for (unsigned i = 1, col = 0; i < n; i++)
        if (c[i])
            shell_printf(sh, "%3u: %-10u%s", i - 1, (unsigned)c[i], ++col % 5 ? " " : "\n");
    shell_printf(sh, "\n");
}

static void on_wifi_password(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (!answer)
        return;
    const char *err = config_check_password(answer);
    if (err) {
        shell_printf(sh, "not changed: %s\n", err);
        return;
    }
    sos_config_t *c = settings();
    strcpy(c->wifi_ssid, pending_ssid);
    strcpy(c->wifi_password, answer);
    memset(answer, 0, strlen(answer));
    save(sh);
    log_msg(SOS_LOG_NOTICE, "config: Wi-Fi network set to '%s'", c->wifi_ssid);
    wifi_reconnect();
}

static void on_wifi_ssid(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (!answer)
        return;
    const char *err = config_check_ssid(answer);
    if (err) {
        shell_printf(sh, "not changed: %s\n", err);
        return;
    }
    strcpy(pending_ssid, answer);
    shell_ask(sh, "Password (empty for an open network): ", true, on_wifi_password, NULL);
}

static void cmd_wifi(shell_t *sh, char *args) {
    if (!args || !*args) {
        wifi_show(sh);
    } else if (!strcmp(args, "set")) {
        if (require_admin(sh))
            shell_ask(sh, "SSID: ", false, on_wifi_ssid, NULL);
    } else if (!strcmp(args, "connect")) {
        if (require_admin(sh))
            wifi_reconnect();
    } else if (!strcmp(args, "keepalive on") || !strcmp(args, "keepalive off")) {
        // For testing only, so not saved: it's back on after a reboot.
        if (!require_admin(sh))
            return;
        wifi_set_keepalive(!strcmp(args, "keepalive on"));
        log_msg(SOS_LOG_NOTICE, "wifi: keepalive %s (until reboot)", wifi_keepalive() ? "on" : "off");
    } else if (!strcmp(args, "stats") || !strcmp(args, "stats chip")) {
        wifi_stats(sh, args[5] != 0);
    } else if (!strncmp(args, "iovar ", 6)) {
        // Debugging only: `wifi iovar <name> [value]` reads or writes a chip variable.
        if (!require_admin(sh))
            return;
        char *name = args + 6, *val = strchr(name, ' ');
        if (val)
            *val++ = '\0';
        uint32_t v = val ? (uint32_t)strtoul(val, NULL, 0) : 0;
        int r = val ? wifi_iovar_set(name, v) : wifi_iovar_get(name, &v);
        shell_printf(sh, "%s %s: result %d, value %u (0x%x)\n", val ? "set" : "get", name, r,
                     (unsigned)v, (unsigned)v);
    } else if (!strncmp(args, "country ", 8)) {
        if (!require_admin(sh))
            return;
        const char *v = !strcmp(args + 8, "worldwide") ? "" : args + 8;
        const char *err = config_check_country(v);
        if (err) {
            shell_printf(sh, "not changed: %s\n", err);
            return;
        }
        snprintf(settings()->wifi_country, sizeof(settings()->wifi_country), "%s", v);
        save(sh);
        log_msg(SOS_LOG_NOTICE, "config: wifi country %s", v[0] ? v : "worldwide");
        shell_printf(sh, "takes effect at the next boot ('reboot')\n");
    } else if (!strcmp(args, "power")) {
        shell_printf(sh, "power saving: %s\n", settings()->wifi_powersave ? "on" : "off");
    } else if (!strcmp(args, "power on") || !strcmp(args, "power off")) {
        if (!require_admin(sh))
            return;
        settings()->wifi_powersave = !strcmp(args, "power on");
        save(sh);
        wifi_apply_power();
        log_msg(SOS_LOG_NOTICE, "config: wifi power saving %s", settings()->wifi_powersave ? "on" : "off");
    } else {
        shell_printf(sh, "usage: wifi [set|connect|stats [chip]|power [on|off]|country <XX>|worldwide|keepalive on|off|\n            iovar <name> [<value>]]\n");
    }
}

// --- hostname, user, config ----------------------------------------------------------------------

static void set_field(shell_t *sh, const char *what, char *value, char *field, size_t size,
                      const char *(*check)(const char *)) {
    if (!value || !*value) {
        shell_printf(sh, "%s: %s\n", what, field);
        return;
    }
    if (!require_admin(sh))
        return;
    const char *err = check(value);
    if (err) {
        shell_printf(sh, "not changed: %s\n", err);
        return;
    }
    snprintf(field, size, "%s", value);
    save(sh);
    log_msg(SOS_LOG_NOTICE, "config: %s set to '%s'", what, field);
    if (field == settings()->hostname)
        shell_printf(sh, "takes effect on the next Wi-Fi connect ('wifi connect')\n");
}

static void on_reset_confirm(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (!answer || strcmp(answer, "yes")) {
        shell_printf(sh, "not reset\n");
        return;
    }
    bool erased = settings_reset();
    // The live configuration is gone either way, the owner secret with it: an encrypted card
    // doesn't stay open under it.
    sdcard_owner_gone();
    if (erased)
        shell_printf(sh, "configuration erased: no Wi-Fi network and no trusted keys; run "
                         "'setup'\n");
    else
        shell_printf(sh, "error: erasing the saved configuration failed (see dmesg). It's "
                         "cleared in memory, but\nthe saved one is still in flash and comes "
                         "back at the next boot.\n");
    wifi_apply_power();
    timesync_apply_cal();               // the RTC goes back to uncalibrated now, not at the next boot
    wifi_reconnect();
}

// --- root secret ---------------------------------------------------------------------------------

static void on_rootsecret_forget(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (!answer || strcmp(answer, "yes")) {
        shell_printf(sh, "the record is kept\n");
        return;
    }
    shell_printf(sh, rootflash_forget() ? "the record is cleared; `reboot` makes a new root secret\n"
                                        : "clearing the record failed (see dmesg)\n");
}

// `rootsecret`: the state of the secret every stored key is derived from (core/keyprov.h);
// `rootsecret forget` (console): clear a record that can't be used, the explicit way out. A
// usable secret can't be cleared here: that would silently lock everything sealed with it.
static void cmd_rootsecret(shell_t *sh, const char *args) {
    int state = rootflash_state();
    if (!args || !*args) {
        if (!rootflash_usable())
            shell_printf(sh, "root secret: none (the firmware image overlaps its flash sector)\n");
        else if (state == ROOTSECRET_OK)
            shell_printf(sh, "root secret: present, in flash (open board: it isn't protected "
                             "from someone who has the board)%s\n",
                         rootflash_degraded() ? "\none of its two copies is damaged; the other "
                                                "is in use" : "");
        else if (state == ROOTSECRET_NONE)
            shell_printf(sh, "root secret: none (making it at boot failed, see dmesg; `reboot` "
                             "tries again)\n");
        else
            shell_printf(sh, "root secret: present but unusable (damaged, or written by newer "
                             "firmware). It's kept as it is:\nnewer firmware may read it. "
                             "`rootsecret forget` clears it; what was stored under it is then "
                             "lost.\n");
        return;
    }
    if (strcmp(args, "forget")) {
        shell_printf(sh, "usage: rootsecret [forget]\n");
        return;
    }
    if (!sh->io->admin) {
        shell_printf(sh, "the root secret can only be cleared on the serial console\n");
        return;
    }
    if (state != ROOTSECRET_INVALID || !rootflash_usable()) {
        shell_printf(sh, state == ROOTSECRET_OK ? "the root secret is usable; it isn't cleared\n"
                                                : "there's no record to clear\n");
        return;
    }
    shell_printf(sh, "This erases the root secret's whole flash sector. Whatever was stored under "
                     "this root\nsecret can't be read again, by any firmware.\n");
    shell_ask(sh, "Clear the unusable record? Type 'yes': ", false, on_rootsecret_forget, NULL);
}

static void cmd_config(shell_t *sh, char *args) {
    if (args && !strcmp(args, "reset")) {
        if (require_admin(sh)) {
            shell_printf(sh, "This also destroys the owner secret that unlocks this board's "
                             "encrypted SD cards.\nCards it encrypted can then only be read on "
                             "a PC, with the key shown when each was\nformatted. Setting the "
                             "board up again does not bring them back.\n");
            shell_ask(sh, "Erase Wi-Fi settings, trusted keys and the owner secret? Type 'yes': ",
                      false, on_reset_confirm, NULL);
        }
        return;
    }
    if (args && *args) {
        shell_printf(sh, "usage: config [reset]\n");
        return;
    }
    const sos_config_t *c = settings();
    shell_printf(sh, "hostname:      %s\n", c->hostname);
    shell_printf(sh, "console:       %s\n", settings_console_has_password()
                                              ? "password set" : "no password (`passwd` sets one)");
    wifi_show(sh);
    shell_printf(sh, "keys:          %u trusted ('key list')\n", c->key_count);
    shell_printf(sh, "nts:           %s%s, ", c->ntp_server[0] ? c->ntp_server : SOS_NTS_SERVER,
                 c->ntp_server[0] ? "" : " (build default)");
    if (c->ntp_sync_hours)
        shell_printf(sh, "sync every %u h\n", (unsigned)c->ntp_sync_hours);
    else
        shell_printf(sh, "periodic sync off\n");
    char cal[40];
    timesync_cal_str(cal, sizeof(cal));
    shell_printf(sh, "rtc cal:       %s\n", cal);
    if (!settings_persistent())
        shell_printf(sh, "warning:       flash storage unavailable; changes aren't saved\n");
}

// --- the console's lock (shell/login.h) ------------------------------------------------------------

static void cmd_setup(shell_t *sh);
static void ask_new_password(shell_t *sh, void (*then)(shell_t *sh));

static uint64_t console_now_ms(void) { return to_ms_since_boot(get_absolute_time()); }
static int console_check_step(void) {
    int r = settings_console_check_step();
    return r == 0 ? LOGIN_MORE : r == 1 ? LOGIN_RIGHT : LOGIN_WRONG;
}

// With no usable root secret nothing sealed can be read, the password included. The console
// must not be open because of that: it stays locked, with only the commands that deal with it.
static const char *console_no_keys(void) {
    return rootflash_state() == ROOTSECRET_OK
               ? NULL
               : "this board's root secret can't be used, so nothing it stores can be read.\n"
                 "`rootsecret` says more, `rootsecret forget` starts over (what was stored is "
                 "lost), `reboot` restarts.";
}

static bool console_rescue_allowed(const char *cmd) {
    return !strcmp(cmd, "rootsecret") || !strcmp(cmd, "rootsecret forget") || !strcmp(cmd, "reboot");
}

static void console_opened(shell_t *sh) {
    log_msg(SOS_LOG_NOTICE, "console: logged in");
    shell_greet(sh);
}

// No console password is set: a new board, a configuration from before console passwords, or
// one provisioned without a password. Whoever is at the console sets it now.
static void console_first_use(shell_t *sh) {
    shell_greet(sh);
    if (!settings()->wifi_ssid[0] && !settings()->key_count) {
        cmd_setup(sh);                  // a board that was never set up: the whole of setup
        return;
    }
    shell_printf(sh, "This board has no console password yet. It's asked for before the console "
                     "takes any\ncommand. Ctrl-C leaves the console without one.\n");
    ask_new_password(sh, NULL);
}

// Not printed on the console while it's locked (the log is held); `dmesg` in an SSH session and
// the console after the next login show it.
static void console_failed(unsigned failures) {
    log_msg(SOS_LOG_WARNING, "console: wrong password (%u since boot)", failures);
}

static const login_hooks_t console_login = {
    console_now_ms, settings_console_has_password, settings_console_check_begin,
    console_check_step, settings_console_check_abort, console_no_keys, console_rescue_allowed,
    console_opened, console_first_use, console_failed,
};

void shell_console_start(shell_t *console) {
    if (!console_no_keys() && !settings_console_has_password())
        log_msg(SOS_LOG_WARNING, "console: no console password is set: the console is open to "
                                 "anyone at the port (`passwd` sets one)");
    login_start(console, &console_login);
}

// --- setup: guided configuration (console only) --------------------------------------------------

static struct {
    char ssid[CONFIG_SSID_MAX + 1];
    char password[CONFIG_PASSWORD_MAX + 1];
    bool wifi_changed;
} setup_state;

static void setup_finish(shell_t *sh) {
    sos_config_t *c = settings();
    bool wifi_changed = setup_state.wifi_changed;
    if (wifi_changed) {
        strcpy(c->wifi_ssid, setup_state.ssid);
        strcpy(c->wifi_password, setup_state.password);
    }
    memset(&setup_state, 0, sizeof(setup_state));
    save(sh);
    log_msg(SOS_LOG_NOTICE, "config: setup completed");
    shell_printf(sh, "\nsaved:\n");
    cmd_config(sh, NULL);
    if (!c->key_count)
        shell_printf(sh, "\nno trusted keys yet: add one with 'key add' to allow SSH logins\n");
    if (wifi_changed)
        wifi_reconnect();           // a new network: don't drop a working link otherwise
}

static void on_setup_key(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (!answer) {
        shell_printf(sh, "setup cancelled; nothing changed\n");
        return;
    }
    if (*answer && !key_add_line(sh, answer)) {
        shell_ask(sh, "Public key to trust (empty to skip): ", false, on_setup_key, NULL);
        return;                                 // try again
    }
    setup_finish(sh);
}

static void ask_setup_key(shell_t *sh) {
    shell_printf(sh, "\nTrusted keys: %u. Paste a public key to trust (one line from\n"
                     "~/.ssh/id_ed25519.pub on your computer), or press Enter to skip.\n",
                 settings()->key_count);
    shell_ask(sh, "Public key: ", false, on_setup_key, NULL);
}

static void on_setup_password(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (!answer) {
        shell_printf(sh, "setup cancelled; nothing changed\n");
        return;
    }
    const char *err = config_check_password(answer);
    if (err) {
        shell_printf(sh, "%s\n", err);
        shell_ask(sh, "Wi-Fi password (empty for an open network): ", true, on_setup_password, NULL);
        return;
    }
    strcpy(setup_state.password, answer);
    memset(answer, 0, strlen(answer));
    setup_state.wifi_changed = true;
    ask_setup_key(sh);
}

static void on_setup_ssid(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (!answer) {
        shell_printf(sh, "setup cancelled; nothing changed\n");
        return;
    }
    if (!*answer && settings()->wifi_ssid[0]) {      // keep the current network
        ask_setup_key(sh);
        return;
    }
    const char *err = config_check_ssid(answer);
    if (err) {
        shell_printf(sh, "%s\n", err);
        shell_ask(sh, "Wi-Fi network (SSID): ", false, on_setup_ssid, NULL);
        return;
    }
    strcpy(setup_state.ssid, answer);
    shell_ask(sh, "Wi-Fi password (empty for an open network): ", true, on_setup_password, NULL);
}

// --- the console password ------------------------------------------------------------------------

// Asking for a new password, twice, then what follows (NULL: nothing).
static char new_password[CONFIG_CONSOLE_PW_MAX + 1];
static void (*after_password)(shell_t *sh);

static void wipe_text(char *s, size_t size) {
    volatile char *v = s;
    while (size--)
        *v++ = 0;
}

static void on_new_password(shell_t *sh, char *answer, void *arg);

static void ask_new_password(shell_t *sh, void (*then)(shell_t *sh)) {
    after_password = then;
    shell_ask(sh, "New console password (8 to 64 characters): ", true, on_new_password, NULL);
}

static void on_new_password_again(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    void (*then)(shell_t *sh) = after_password;
    if (!answer) {
        wipe_text(new_password, sizeof(new_password));
        shell_printf(sh, "cancelled; the console password is unchanged\n");
        return;
    }
    bool same = !strcmp(answer, new_password);
    wipe_text(answer, strlen(answer));
    if (!same) {
        wipe_text(new_password, sizeof(new_password));
        shell_printf(sh, "they differ; try again\n");
        ask_new_password(sh, then);
        return;
    }
    const char *err = settings_console_set_password(new_password);
    wipe_text(new_password, sizeof(new_password));
    if (err) {
        shell_printf(sh, "not set: %s\n", err);
        return;
    }
    save(sh);
    log_msg(SOS_LOG_NOTICE, "config: console password set");
    shell_printf(sh, "console password set; the console asks for it from now on (`logout` locks "
                     "it)\n");
    if (then)
        then(sh);
}

static void on_new_password(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (!answer) {
        shell_printf(sh, "cancelled; the console password is unchanged\n");
        return;
    }
    const char *err = config_check_console_password(answer);
    if (err) {
        wipe_text(answer, strlen(answer));
        shell_printf(sh, "%s\n", err);
        ask_new_password(sh, after_password);
        return;
    }
    strcpy(new_password, answer);
    wipe_text(answer, strlen(answer));
    shell_ask(sh, "Again: ", true, on_new_password_again, NULL);
}

// The current password, checked in one go (a few hundred milliseconds), before changing it: an
// unlocked console left alone shouldn't be enough to change the password.
static void on_current_password(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (!answer)
        return;
    int r = settings_console_check_begin(answer) ? 0 : -1;
    wipe_text(answer, strlen(answer));
    while (r == 0)
        r = settings_console_check_step();
    if (r != 1) {
        shell_printf(sh, "wrong password\n");
        log_msg(SOS_LOG_WARNING, "console: wrong current password given to `passwd`");
        return;
    }
    ask_new_password(sh, NULL);
}

static void cmd_passwd(shell_t *sh) {
    if (!sh->io->admin) {
        shell_printf(sh, "the console password can only be changed on the serial console\n");
        return;
    }
    if (settings_console_has_password())
        shell_ask(sh, "Current console password: ", true, on_current_password, NULL);
    else
        ask_new_password(sh, NULL);
}

static void setup_network(shell_t *sh);

static void cmd_setup(shell_t *sh) {
    if (!require_admin(sh))
        return;
    shell_printf(sh, "SOS setup. Press Enter to keep the value in brackets, Ctrl-C to cancel.\n");
    // The console password comes first: a board isn't left set up with an open console.
    if (!settings_console_has_password()) {
        shell_printf(sh, "This board has no console password yet. It's asked for before the "
                         "console takes any\ncommand. If it's forgotten, the only way back in is "
                         "to erase the configuration over USB.\n");
        ask_new_password(sh, setup_network);
        return;
    }
    setup_network(sh);
}

static void setup_network(shell_t *sh) {
    static char prompt[CONFIG_SSID_MAX + 40];
    memset(&setup_state, 0, sizeof(setup_state));
    if (settings()->wifi_ssid[0])
        snprintf(prompt, sizeof(prompt), "Wi-Fi network (SSID) [%s]: ", settings()->wifi_ssid);
    else
        snprintf(prompt, sizeof(prompt), "Wi-Fi network (SSID): ");
    shell_ask(sh, prompt, false, on_setup_ssid, NULL);
}

static void show_ntp_sync_hours(shell_t *sh) {
    unsigned h = settings()->ntp_sync_hours;
    if (h)
        shell_printf(sh, "ntp sync: every %u h\n", h);
    else
        shell_printf(sh, "ntp sync: off (RTC only; 'time sync' syncs once)\n");
}

// `time auto [off | <hours>]`: periodic NTS sync (a setting, so console only to change).
static void cmd_time_auto(shell_t *sh, const char *value) {
    while (*value == ' ')
        value++;
    if (!*value) {
        show_ntp_sync_hours(sh);
        return;
    }
    if (!require_admin(sh))
        return;
    unsigned long hours = 0;
    if (strcmp(value, "off")) {
        char *end;
        hours = strtoul(value, &end, 10);
        if (*end || end == value || hours < 1 || hours > CONFIG_NTP_SYNC_HOURS_MAX) {
            shell_printf(sh, "not changed: give 'off' or hours from 1 to %u\n",
                         CONFIG_NTP_SYNC_HOURS_MAX);
            return;
        }
    }
    settings()->ntp_sync_hours = (uint16_t)hours;
    save(sh);
    if (hours)
        log_msg(SOS_LOG_NOTICE, "config: ntp sync every %lu h", hours);
    else
        log_msg(SOS_LOG_NOTICE, "config: ntp sync off");
    show_ntp_sync_hours(sh);
}

// `time cal [off | <ppm>]`: RTC calibration (a setting, so console only to change). The value is
// how fast the RTC runs, in ppm (negative: slow); the RTC is then slowed, or sped up, by the
// nearest amount its offset register can do.
static void cmd_time_cal(shell_t *sh, const char *value) {
    char cal[40];
    while (*value == ' ')
        value++;
    if (!*value) {
        timesync_cal_str(cal, sizeof(cal));
        shell_printf(sh, "rtc calibration: %s\n", cal);
        return;
    }
    if (!require_admin(sh))
        return;
    int32_t mppm = 0;
    int steps = 0;
    if (strcmp(value, "off") &&
        (!timeconv_parse_ppm(value, &mppm) || !timeconv_cal_steps(mppm, &steps))) {
        shell_printf(sh, "not changed: give 'off', or how fast the RTC runs in ppm, from %d to %d "
                         "(negative: slow)\n",
                     TIMECONV_CAL_STEPS_MIN * TIMECONV_CAL_STEP_MPPM / 1000,
                     TIMECONV_CAL_STEPS_MAX * TIMECONV_CAL_STEP_MPPM / 1000);
        return;
    }
    settings()->rtc_offset = (int8_t)steps;
    save(sh);
    bool applied = timesync_apply_cal();
    timesync_cal_str(cal, sizeof(cal));
    log_msg(SOS_LOG_NOTICE, "config: rtc calibration %s", cal);
    shell_printf(sh, "rtc calibration: %s%s\n", cal,
                 applied ? "" : " (saved, but the RTC didn't take it)");
}

static void show_ntp_server(shell_t *sh) {
    const char *s = settings()->ntp_server;
    if (*s)
        shell_printf(sh, "nts server: %s\n", s);
    else
        shell_printf(sh, "nts server: %s (build default)\n", SOS_NTS_SERVER);
}

// `time server [<name> | default]`: NTS server (a setting, so console only to change). Its
// certificate chain must end at a built-in root (src/tls/tls_roots.c).
static void cmd_time_server(shell_t *sh, const char *value) {
    while (*value == ' ')
        value++;
    if (!*value) {
        show_ntp_server(sh);
        return;
    }
    if (!require_admin(sh))
        return;
    sos_config_t *c = settings();
    if (!strcmp(value, "default")) {
        c->ntp_server[0] = '\0';
    } else {
        const char *err = config_check_ntp_server(value);
        if (err) {
            shell_printf(sh, "not changed: %s\n", err);
            return;
        }
        snprintf(c->ntp_server, sizeof(c->ntp_server), "%s", value);
    }
    save(sh);
    log_msg(SOS_LOG_NOTICE, "config: nts server set to '%s'",
            c->ntp_server[0] ? c->ntp_server : SOS_NTS_SERVER);
    show_ntp_server(sh);
    shell_printf(sh, "used from the next sync ('time sync' to sync now)\n");
}

// Clock sync is allowed from SSH too: it can't lock anyone out, and the RTC drifts.
static void cmd_time(shell_t *sh, char *args) {
    if (!args)
        timesync_status(sh);
    else if (!strcmp(args, "sync"))
        shell_printf(sh, timesync_request_nts()
                     ? "nts: sync started; 'time' shows the result\n"
                     : "nts: a sync is already running\n");
    else if (!strcmp(args, "sync sntp")) {
        // Unauthenticated: whoever answers sets the clock, which TLS checks certificate dates
        // against. So only on the console, like the configuration.
        if (!require_admin(sh))
            return;
        shell_printf(sh, timesync_request_sntp()
                     ? "sntp: unauthenticated sync from " SOS_NTP_SERVER
                       " started; 'time' shows the result\n"
                     : "a sync is already running\n");
    } else if (!strncmp(args, "auto", 4) && (args[4] == ' ' || !args[4]))
        cmd_time_auto(sh, args + 4);
    else if (!strncmp(args, "server", 6) && (args[6] == ' ' || !args[6]))
        cmd_time_server(sh, args + 6);
    else if (!strncmp(args, "cal", 3) && (args[3] == ' ' || !args[3]))
        cmd_time_cal(sh, args + 3);
    else
        shell_printf(sh, "usage: time [sync [sntp] | auto [off | <hours>] | server [<name> | default] | "
                         "cal [off | <ppm>]]\n");
}

// `nts` / `nts ke [host]` / `nts query`: NTS key exchange and authenticated time queries
// (development: queries report the offset but don't set the clock yet).
// `tls roots [approve | forget]`: extra TLS roots from the SD card (src/tls/certstore.c).
static void cmd_tls(shell_t *sh, char *args) {
    if (args && !strncmp(args, "roots", 5) && (args[5] == ' ' || !args[5])) {
        char *rest = args + 5;
        while (*rest == ' ')
            rest++;
        certstore_command(sh, *rest ? rest : NULL);
    } else {
        shell_printf(sh, "usage: tls roots [approve | forget]\n");
    }
}

static void cmd_nts(shell_t *sh, char *args) {
    if (!args) {
        nts_status(sh);
        return;
    }
    if (!strcmp(args, "query")) {
        const char *err = nts_query_start();
        if (err)
            shell_printf(sh, "nts: %s\n", err);
        else
            shell_printf(sh, "nts: query started; 'nts' shows the result\n");
        return;
    }
    if (strncmp(args, "ke", 2) || (args[2] != ' ' && args[2])) {
        shell_printf(sh, "usage: nts [ke [<host>] | query]\n");
        return;
    }
    const char *host = args + 2;
    while (*host == ' ')
        host++;
    // Another host makes the device connect out to it: console only. A session may run the
    // exchange with the build-time default server (SOS_NTS_SERVER).
    if (*host && !require_admin(sh))
        return;
    if (*host && config_check_ntp_server(host)) {
        shell_printf(sh, "bad host name: %s\n", config_check_ntp_server(host));
        return;
    }
    shell_printf(sh, nts_ke_start(*host ? host : NULL)
                 ? "nts: key exchange started; 'nts' shows the result\n"
                 : "nts: a key exchange or query is already running\n");
}

// --- dispatch ------------------------------------------------------------------------------------

void shell_execute(shell_t *sh, char *cmd) {
    // The console takes no command while it's locked (shell/login.h).
    if (sh->io->admin && login_gate(sh, cmd))
        return;
    // Split "command arguments"; args is NULL without arguments.
    char *args = strchr(cmd, ' ');
    if (args) {
        *args++ = '\0';
        while (*args == ' ')
            args++;
        if (!*args)
            args = NULL;
    }

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?"))
        cmd_help(sh);
    else if (!strcmp(cmd, "uptime"))
        cmd_uptime(sh);
    else if (!strcmp(cmd, "version"))
        cmd_version(sh);
    else if (!strcmp(cmd, "time"))
        cmd_time(sh, args);
    else if (!strcmp(cmd, "tls"))
        cmd_tls(sh, args);
    else if (!strcmp(cmd, "nts"))
        cmd_nts(sh, args);
    else if (!strcmp(cmd, "mem"))
        cmd_mem(sh);
    else if (!strcmp(cmd, "net"))
        cmd_net(sh);
    else if (!strcmp(cmd, "ssh"))
        sshd_status(sh);
    else if (!strcmp(cmd, "who"))
        chat_cmd_who(sh, args);
    else if (!strcmp(cmd, "wall"))
        chat_cmd_wall(sh, args);
    else if (!strcmp(cmd, "chat"))
        chat_cmd_chat(sh, args);
    else if (!strcmp(cmd, "dmesg"))
        cmd_dmesg(sh);
    else if (!strcmp(cmd, "log"))
        cmd_log(sh, args);
    else if (!strcmp(cmd, "config"))
        cmd_config(sh, args);
    else if (!strcmp(cmd, "rootsecret"))
        cmd_rootsecret(sh, args);
    else if (!strcmp(cmd, "key"))
        cmd_key(sh, args);
    else if (!strcmp(cmd, "wifi"))
        cmd_wifi(sh, args);
    else if (!strcmp(cmd, "hostname"))
        set_field(sh, "hostname", args, settings()->hostname, sizeof(settings()->hostname),
                  config_check_hostname);
    else if (!strcmp(cmd, "sd"))
        sdcard_command(sh, args);
    else if (!strcmp(cmd, "lua"))
        lua_command(sh, args);
    else if (!strcmp(cmd, "edit"))
        edit_command(sh, args);
    else if (!strcmp(cmd, "http"))
        http_command(sh, args);
    else if (!strcmp(cmd, "passwd"))
        cmd_passwd(sh);
    else if (!strcmp(cmd, "setup"))
        cmd_setup(sh);
    else if (!strcmp(cmd, "logout") || !strcmp(cmd, "exit"))
        shell_logout(sh);
    else if (!strcmp(cmd, "crash")) {
        // Testing the fault record (core/fault.h): stops the firmware on purpose.
        if (!require_admin(sh))
            return;
        if (args)
            fault_test(args);
        shell_printf(sh, "usage: crash fault|stack|panic|hang  (testing: stops the firmware)\n");
    } else if (!strcmp(cmd, "reboot")) {
        shell_printf(sh, "rebooting...\n");
        sleep_ms(100);
        watchdog_reboot(0, 0, 0);
    } else if (!files_command(sh, cmd, args))
        shell_printf(sh, "%s: unknown command (try 'help')\n", cmd);
}
