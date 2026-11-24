// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "shell/shell.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "lwip/netif.h"

#include "core/config.h"
#include "core/fault.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/stackmon.h"
#include "core/wdog.h"
#include "files/files.h"
#include "files/sdcard.h"
#include "lua/lua_core1.h"
#include "lua/lua_host.h"
#include "net/http.h"
#include "net/wifi.h"
#include "ssh/sshd.h"
#include "time/nts.h"
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

static void cmd_help(shell_t *sh) {
    shell_printf(sh, "help              this list\n");
    shell_printf(sh, "uptime            time since boot\n");
    shell_printf(sh, "time [sync]       date and time (UTC) / set clock and RTC from NTS\n");
    shell_printf(sh, "time auto|server  periodic sync / NTS server settings\n");
    shell_printf(sh, "nts [ke]          NTS status / key exchange (development)\n");
    shell_printf(sh, "nts query         authenticated NTS time query; reports the offset\n");
    shell_printf(sh, "tls roots         trusted root certificates (built-in and SD card)\n");
    shell_printf(sh, "mem               heap and stack usage\n");
    shell_printf(sh, "net               Wi-Fi and IP status\n");
    shell_printf(sh, "ssh               SSH server status\n");
    shell_printf(sh, "dmesg             system log\n");
    shell_printf(sh, "log               log levels\n");
    shell_printf(sh, "config            show the configuration\n");
    shell_printf(sh, "key [list]        trusted SSH public keys\n");
    shell_printf(sh, "wifi              Wi-Fi network\n");
    shell_printf(sh, "sd [info]         SD card status (also: sd speed)\n");
    files_help(sh);
    shell_printf(sh, "lua <file> | lua -e <code>  run a Lua script (on core 1; Ctrl-C stops)\n");
    shell_printf(sh, "lua               interactive Lua prompt (Ctrl-D or exit leaves)\n");
    if (sh->io->admin) {
        shell_printf(sh, "setup             guided configuration\n");
        shell_printf(sh, "log console|history <level>  set log levels (until reboot)\n");
        shell_printf(sh, "time sync sntp    one unauthenticated SNTP sync (" SOS_NTP_SERVER ")\n");
        shell_printf(sh, "nts ke <host>     key exchange with another server (development)\n");
        shell_printf(sh, "http get|head <url>, http post <url> <text>  HTTPS request (testing)\n");
        shell_printf(sh, "wifi set|connect  change the network / reconnect\n");
        shell_printf(sh, "wifi power on|off Wi-Fi power saving (default off)\n");
        shell_printf(sh, "wifi keepalive on|off  testing: stop announcing the address (until reboot)\n");
        shell_printf(sh, "key add [line]    trust a public key (paste an authorized_keys line)\n");
        shell_printf(sh, "key del <n>       stop trusting key n\n");
        shell_printf(sh, "hostname <name>   set the hostname\n");
        shell_printf(sh, "user <name>       set the SSH user name\n");
        shell_printf(sh, "time auto <h>|off sync the clock from NTS every h hours (default off)\n");
        shell_printf(sh, "time server <name>|default  set the NTS server\n");
        shell_printf(sh, "tls roots approve|forget  trust /certs/roots.pem for TLS / stop\n");
        shell_printf(sh, "config reset      erase the configuration\n");
        shell_printf(sh, "crash fault|stack|panic|hang  testing: stop the firmware that way\n");
        shell_printf(sh, "sd format [exfat|fat32]  erase and format the SD card\n");
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
    extern char __end__, __HeapLimit;
    struct mallinfo mi = mallinfo();
    size_t heap_total = (size_t)(&__HeapLimit - &__end__);

    shell_printf(sh, "heap:  %u in use, %u peak, %u total\n",
                 (unsigned)mi.uordblks, (unsigned)mi.arena, (unsigned)heap_total);
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

static void wifi_show(shell_t *sh) {
    const sos_config_t *c = settings();
    shell_printf(sh, "network:  %s\n", c->wifi_ssid[0] ? c->wifi_ssid : "(not configured)");
    shell_printf(sh, "password: %s\n", c->wifi_password[0] ? "set" : "none (open network)");
    shell_printf(sh, "state:    %s\n", wifi_state());
    shell_printf(sh, "power:    saving %s\n", c->wifi_powersave ? "on" : "off");
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
        shell_printf(sh, "usage: wifi [set|connect|power [on|off]|keepalive on|off]\n");
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
    if (!settings_reset())
        shell_printf(sh, "error: erasing the configuration failed\n");
    shell_printf(sh, "configuration erased: no Wi-Fi network and no trusted keys; run 'setup'\n");
    wifi_apply_power();
    wifi_reconnect();
}

static void cmd_config(shell_t *sh, char *args) {
    if (args && !strcmp(args, "reset")) {
        if (require_admin(sh))
            shell_ask(sh, "Erase Wi-Fi settings, user and trusted keys? Type 'yes': ", false,
                      on_reset_confirm, NULL);
        return;
    }
    if (args && *args) {
        shell_printf(sh, "usage: config [reset]\n");
        return;
    }
    const sos_config_t *c = settings();
    shell_printf(sh, "hostname: %s\n", c->hostname);
    shell_printf(sh, "ssh user: %s\n", c->ssh_user);
    wifi_show(sh);
    shell_printf(sh, "keys:     %u trusted ('key list')\n", c->key_count);
    shell_printf(sh, "nts:      %s%s, ", c->ntp_server[0] ? c->ntp_server : SOS_NTS_SERVER,
                 c->ntp_server[0] ? "" : " (build default)");
    if (c->ntp_sync_hours)
        shell_printf(sh, "sync every %u h\n", (unsigned)c->ntp_sync_hours);
    else
        shell_printf(sh, "periodic sync off\n");
    if (!settings_persistent())
        shell_printf(sh, "warning:  flash storage unavailable; changes aren't saved\n");
}

// --- setup: guided configuration (console only) --------------------------------------------------

static struct {
    char ssid[CONFIG_SSID_MAX + 1];
    char password[CONFIG_PASSWORD_MAX + 1];
    char user[CONFIG_USER_MAX + 1];
    bool wifi_changed;
} setup_state;

static void setup_finish(shell_t *sh) {
    sos_config_t *c = settings();
    bool wifi_changed = setup_state.wifi_changed;
    if (wifi_changed) {
        strcpy(c->wifi_ssid, setup_state.ssid);
        strcpy(c->wifi_password, setup_state.password);
    }
    strcpy(c->ssh_user, setup_state.user);
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

static void on_setup_user(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (!answer) {
        shell_printf(sh, "setup cancelled; nothing changed\n");
        return;
    }
    if (*answer) {
        const char *err = config_check_user(answer);
        if (err) {
            shell_printf(sh, "%s\n", err);
            shell_ask(sh, "SSH user name: ", false, on_setup_user, NULL);
            return;
        }
        strcpy(setup_state.user, answer);
    }
    shell_printf(sh, "\nTrusted keys: %u. Paste a public key to trust (one line from\n"
                     "~/.ssh/id_ed25519.pub on your computer), or press Enter to skip.\n",
                 settings()->key_count);
    shell_ask(sh, "Public key: ", false, on_setup_key, NULL);
}

static void ask_setup_user(shell_t *sh) {
    static char prompt[CONFIG_USER_MAX + 32];
    snprintf(prompt, sizeof(prompt), "SSH user name [%s]: ", setup_state.user);
    shell_ask(sh, prompt, false, on_setup_user, NULL);
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
    ask_setup_user(sh);
}

static void on_setup_ssid(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (!answer) {
        shell_printf(sh, "setup cancelled; nothing changed\n");
        return;
    }
    if (!*answer && settings()->wifi_ssid[0]) {      // keep the current network
        ask_setup_user(sh);
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

static void cmd_setup(shell_t *sh) {
    static char prompt[CONFIG_SSID_MAX + 40];
    if (!require_admin(sh))
        return;
    memset(&setup_state, 0, sizeof(setup_state));
    strcpy(setup_state.user, settings()->ssh_user);
    shell_printf(sh, "SOS setup. Press Enter to keep the value in brackets, Ctrl-C to cancel.\n");
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
    else
        shell_printf(sh, "usage: time [sync [sntp] | auto [off | <hours>] | server [<name> | default]]\n");
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
    else if (!strcmp(cmd, "dmesg"))
        cmd_dmesg(sh);
    else if (!strcmp(cmd, "log"))
        cmd_log(sh, args);
    else if (!strcmp(cmd, "config"))
        cmd_config(sh, args);
    else if (!strcmp(cmd, "key"))
        cmd_key(sh, args);
    else if (!strcmp(cmd, "wifi"))
        cmd_wifi(sh, args);
    else if (!strcmp(cmd, "hostname"))
        set_field(sh, "hostname", args, settings()->hostname, sizeof(settings()->hostname),
                  config_check_hostname);
    else if (!strcmp(cmd, "user"))
        set_field(sh, "ssh user", args, settings()->ssh_user, sizeof(settings()->ssh_user),
                  config_check_user);
    else if (!strcmp(cmd, "sd"))
        sdcard_command(sh, args);
    else if (!strcmp(cmd, "lua"))
        lua_command(sh, args);
    else if (!strcmp(cmd, "http"))
        http_command(sh, args);
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
