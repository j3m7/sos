// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "shell.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "lwip/netif.h"

#include "sshd.h"
#include "stackmon.h"
#include "wdog.h"

// --- commands ------------------------------------------------------------------

static void cmd_help(shell_t *sh) {
    shell_printf(sh, "help      this list\n");
    shell_printf(sh, "uptime    time since boot\n");
    shell_printf(sh, "mem       heap and stack usage\n");
    shell_printf(sh, "net       Wi-Fi and IP status\n");
    shell_printf(sh, "ssh       SSH server status\n");
    shell_printf(sh, "logout    end this session (also Ctrl-D)\n");
    shell_printf(sh, "reboot    restart the device\n");
}

static void cmd_uptime(shell_t *sh) {
    uint64_t s = to_us_since_boot(get_absolute_time()) / 1000000;
    shell_printf(sh, "up %llud %02llu:%02llu:%02llu%s\n",
                 s / 86400, (s / 3600) % 24, (s / 60) % 60, s % 60,
                 wdog_caused_reset() ? " (last reset: watchdog)" : "");
}

static void cmd_mem(shell_t *sh) {
    extern char __end__, __HeapLimit;
    struct mallinfo mi = mallinfo();
    size_t heap_total = (size_t)(&__HeapLimit - &__end__);

    shell_printf(sh, "heap:  %u in use, %u peak, %u total\n",
                 (unsigned)mi.uordblks, (unsigned)mi.arena, (unsigned)heap_total);
    shell_printf(sh, "stack: %u peak of %u bytes\n",
                 (unsigned)stackmon_peak(), (unsigned)stackmon_size());
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
}


void shell_execute(shell_t *sh, char *cmd) {
    if (!strcmp(cmd, "help") || !strcmp(cmd, "?"))
        cmd_help(sh);
    else if (!strcmp(cmd, "uptime"))
        cmd_uptime(sh);
    else if (!strcmp(cmd, "mem"))
        cmd_mem(sh);
    else if (!strcmp(cmd, "net"))
        cmd_net(sh);
    else if (!strcmp(cmd, "ssh"))
        sshd_status(sh);
    else if (!strcmp(cmd, "logout") || !strcmp(cmd, "exit"))
        shell_logout(sh);
    else if (!strcmp(cmd, "reboot")) {
        shell_printf(sh, "rebooting...\n");
        sleep_ms(100);
        watchdog_reboot(0, 0, 0);
    } else
        shell_printf(sh, "%s: unknown command (try 'help')\n", cmd);
}
