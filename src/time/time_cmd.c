// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

// The `time` and `nts` commands (moved from shell/shell.c, docs/design/build-features.md §4.1).

#include "time/time_cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/config.h"
#include "core/log.h"
#include "core/settings.h"
#include "time/nts.h"
#include "time/timeconv.h"
#include "time/timesync.h"
#include "time/tzzone.h"

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
    if (!shell_require_admin(sh))
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
    shell_save_settings(sh);
    if (hours)
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: ntp sync every %lu h", hours);
    else
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: ntp sync off");
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
    if (!shell_require_admin(sh))
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
    shell_save_settings(sh);
    bool applied = timesync_apply_cal();
    timesync_cal_str(cal, sizeof(cal));
    log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: rtc calibration %s", cal);
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
    if (!shell_require_admin(sh))
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
    shell_save_settings(sh);
    log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: nts server set to '%s'",
            c->ntp_server[0] ? c->ntp_server : SOS_NTS_SERVER);
    show_ntp_server(sh);
    shell_printf(sh, "used from the next sync ('time sync' to sync now)\n");
}

// Clock sync is allowed from SSH too: it can't lock anyone out, and the RTC drifts.
void time_command(shell_t *sh, char *args) {
    if (!args)
        timesync_status(sh);
    else if (!strcmp(args, "sync"))
        shell_printf(sh, timesync_request_nts()
                     ? "nts: sync started; 'time' shows the result\n"
                     : "nts: a sync is already running\n");
    else if (!strcmp(args, "sync sntp")) {
        // Unauthenticated: whoever answers sets the clock, which TLS checks certificate dates
        // against. So only on the console, like the configuration.
        if (!shell_require_admin(sh))
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
    else if (!strncmp(args, "zone", 4) && (args[4] == ' ' || !args[4]))
        tzzone_command(sh, args + 4);
    else
        shell_printf(sh, "usage: time [sync [sntp] | auto [off | <hours>] | server [<name> | default] | "
                         "cal [off | <ppm>] | zone [<name> | utc | list [<prefix>] | install]]\n");
}

// `nts` / `nts ke [host]` / `nts query`: NTS key exchange and authenticated time queries
// (development: queries report the offset but don't set the clock yet).
void nts_command(shell_t *sh, char *args) {
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
    if (*host && !shell_require_admin(sh))
        return;
    if (*host && config_check_ntp_server(host)) {
        shell_printf(sh, "bad host name: %s\n", config_check_ntp_server(host));
        return;
    }
    shell_printf(sh, nts_ke_start(*host ? host : NULL)
                 ? "nts: key exchange started; 'nts' shows the result\n"
                 : "nts: a key exchange or query is already running\n");
}

// NTS servers the board knows: the build's default and the one set with `time server`.
void time_nts_servers(shell_value_fn fn, void *arg) {
    const char *set = settings()->ntp_server;
    fn(NTS_DEFAULT_SERVER, "default", arg);
    if (set[0] && strcmp(set, NTS_DEFAULT_SERVER))
        fn(set, "set with 'time server'", arg);
}

// `nts ke <host>` and `time server <name>`: those servers; console only, as another host is.
static void tab_nts_hosts(shell_t *sh, shell_value_fn fn, void *arg) {
    if (sh->io->admin)
        time_nts_servers(fn, arg);
}

static void tab_time_server(shell_t *sh, shell_value_fn fn, void *arg) {
    if (!sh->io->admin)
        return;
    fn("default", NULL, arg);
    time_nts_servers(fn, arg);
}

static const shell_values_t nts_hosts = SHELL_VALUES_INIT(tab_nts_hosts);
static const shell_values_t time_servers = SHELL_VALUES_INIT(tab_time_server);
static const shell_values_t time_zones = SHELL_VALUES_INIT(tzzone_tab);

// The words Tab offers (shell.h).
#define END {0}
#define W(name) {name, NULL, 0}
const shell_word_t time_words[] = {
    {"sync", (const shell_word_t[]){{"sntp", NULL, SHELL_ADMIN}, END}, 0},
    {"auto", (const shell_word_t[]){{"off", NULL, SHELL_ADMIN}, END}, 0},
    {"server", SHELL_VALUES_OF(time_servers), 0},
    {"cal", (const shell_word_t[]){{"off", NULL, SHELL_ADMIN}, END}, 0},
    {"zone", SHELL_VALUES_OF(time_zones), 0},
    END};
const shell_word_t nts_words[] = {{"ke", SHELL_VALUES_OF(nts_hosts), 0}, W("query"), END};
#undef W
#undef END
