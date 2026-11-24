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

#define LINE_MAX_LEN 80

static char line[LINE_MAX_LEN + 1];
static size_t line_len;
static bool last_was_cr;

static void prompt(void) {
    printf("sos> ");
}

static void cmd_help(void) {
    printf("help      this list\n"
           "uptime    time since boot\n"
           "mem       heap and stack usage\n"
           "net       Wi-Fi and IP status\n"
           "ssh       SSH server status\n"
           "logout    end the SSH session (also Ctrl-D)\n"
           "reboot    restart the device\n");
}

static void cmd_uptime(void) {
    uint64_t s = to_us_since_boot(get_absolute_time()) / 1000000;
    printf("up %llud %02llu:%02llu:%02llu\n", s / 86400, (s / 3600) % 24, (s / 60) % 60, s % 60);
}

static void cmd_mem(void) {
    extern char __end__, __HeapLimit;
    struct mallinfo mi = mallinfo();
    size_t heap_total = (size_t)(&__HeapLimit - &__end__);

    printf("heap:  %u in use, %u peak, %u total\n",
           (unsigned)mi.uordblks, (unsigned)mi.arena, (unsigned)heap_total);
    printf("stack: %u peak of %u bytes\n", (unsigned)stackmon_peak(), (unsigned)stackmon_size());
}

static void cmd_net(void) {
    int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    const char *s = link == CYW43_LINK_UP ? "up" : link == CYW43_LINK_JOIN ? "joined"
                  : link == CYW43_LINK_NOIP ? "no ip" : link == CYW43_LINK_FAIL ? "failed"
                  : link == CYW43_LINK_NONET ? "no network" : link == CYW43_LINK_BADAUTH ? "bad auth"
                  : "down";
    int32_t rssi = 0;
    cyw43_wifi_get_rssi(&cyw43_state, &rssi);

    cyw43_arch_lwip_begin();
    struct netif *n = &cyw43_state.netif[CYW43_ITF_STA];
    printf("wifi:  %s, rssi %ld dBm\n", s, (long)rssi);
    printf("ip:    %s\n", ip4addr_ntoa(netif_ip4_addr(n)));
    printf("gw:    %s\n", ip4addr_ntoa(netif_ip4_gw(n)));
    printf("host:  %s\n", netif_get_hostname(n));
    cyw43_arch_lwip_end();
}

static void execute(char *cmd) {
    while (*cmd == ' ')
        cmd++;
    char *end = cmd + strlen(cmd);
    while (end > cmd && end[-1] == ' ')
        *--end = '\0';

    if (!*cmd)
        return;
    else if (!strcmp(cmd, "help") || !strcmp(cmd, "?"))
        cmd_help();
    else if (!strcmp(cmd, "uptime"))
        cmd_uptime();
    else if (!strcmp(cmd, "mem"))
        cmd_mem();
    else if (!strcmp(cmd, "net"))
        cmd_net();
    else if (!strcmp(cmd, "ssh"))
        sshd_status();
    else if (!strcmp(cmd, "logout") || !strcmp(cmd, "exit"))
        sshd_disconnect();
    else if (!strcmp(cmd, "reboot")) {
        printf("rebooting...\n");
        sleep_ms(100);
        watchdog_reboot(0, 0, 0);
    } else
        printf("%s: unknown command (try 'help')\n", cmd);
}

void shell_init(void) {
    line_len = 0;
    printf("\nType 'help' for commands.\n");
    prompt();
}

void shell_poll(void) {
    int c;
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (c == '\n' && last_was_cr) {      // swallow LF of a CRLF pair
            last_was_cr = false;
            continue;
        }
        last_was_cr = (c == '\r');

        if (c == '\r' || c == '\n') {
            printf("\n");
            line[line_len] = '\0';
            execute(line);
            line_len = 0;
            prompt();
        } else if (c == 0x7f || c == '\b') {
            if (line_len > 0) {
                line_len--;
                printf("\b \b");
            }
        } else if (c == 0x03) {              // Ctrl-C: discard line
            printf("^C\n");
            line_len = 0;
            prompt();
        } else if (c == 0x04) {              // Ctrl-D: logout
            printf("\n");
            sshd_disconnect();
            line_len = 0;
            prompt();
        } else if (c >= 0x20 && c < 0x7f && line_len < LINE_MAX_LEN) {
            line[line_len++] = (char)c;
            putchar(c);
        }
    }
}
