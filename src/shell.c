// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "shell.h"

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "lwip/netif.h"

#include "sshd.h"
#include "stackmon.h"

// --- output --------------------------------------------------------------------

static void write_out(shell_t *sh, const char *buf, size_t len) {
    if (!sh->io->crlf) {
        sh->io->write(sh->ctx, buf, (int)len);
        return;
    }
    // Translate "\n" to "\r\n", writing the runs in between in one go.
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            if (i > start)
                sh->io->write(sh->ctx, buf + start, (int)(i - start));
            sh->io->write(sh->ctx, "\r\n", 2);
            start = i + 1;
        }
    }
    if (len > start)
        sh->io->write(sh->ctx, buf + start, (int)(len - start));
}

void shell_printf(shell_t *sh, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(buf))
        n = sizeof(buf) - 1;    // truncated; callers keep individual lines short
    write_out(sh, buf, (size_t)n);
}

static void prompt(shell_t *sh) {
    shell_printf(sh, "sos> ");
}

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
    shell_printf(sh, "up %llud %02llu:%02llu:%02llu\n",
                 s / 86400, (s / 3600) % 24, (s / 60) % 60, s % 60);
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

static void cmd_logout(shell_t *sh) {
    if (sh->io->logout)
        sh->io->logout(sh->ctx);
    else
        shell_printf(sh, "logout: the %s session can't be ended\n", sh->io->name);
}

static void execute(shell_t *sh, char *cmd) {
    while (*cmd == ' ')
        cmd++;
    char *end = cmd + strlen(cmd);
    while (end > cmd && end[-1] == ' ')
        *--end = '\0';

    if (!*cmd)
        return;
    else if (!strcmp(cmd, "help") || !strcmp(cmd, "?"))
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
        cmd_logout(sh);
    else if (!strcmp(cmd, "reboot")) {
        shell_printf(sh, "rebooting...\n");
        sleep_ms(100);
        watchdog_reboot(0, 0, 0);
    } else
        shell_printf(sh, "%s: unknown command (try 'help')\n", cmd);
}

// --- input ---------------------------------------------------------------------

void shell_init(shell_t *sh, const shell_io_t *io, void *ctx) {
    sh->io = io;
    sh->ctx = ctx;
    sh->line_len = 0;
    sh->last_was_cr = false;
    shell_printf(sh, "\nType 'help' for commands.\n");
    prompt(sh);
}

void shell_poll(shell_t *sh) {
    char buf[32];
    int n;
    while ((n = sh->io->read(sh->ctx, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' && sh->last_was_cr) {      // swallow LF of a CRLF pair
                sh->last_was_cr = false;
                continue;
            }
            sh->last_was_cr = (c == '\r');

            if (c == '\r' || c == '\n') {
                shell_printf(sh, "\n");
                sh->line[sh->line_len] = '\0';
                execute(sh, sh->line);
                sh->line_len = 0;
                prompt(sh);
            } else if (c == 0x7f || c == '\b') {
                if (sh->line_len > 0) {
                    sh->line_len--;
                    shell_printf(sh, "\b \b");
                }
            } else if (c == 0x03) {                  // Ctrl-C: discard line
                shell_printf(sh, "^C\n");
                sh->line_len = 0;
                prompt(sh);
            } else if (c == 0x04) {                  // Ctrl-D on an empty line: logout
                if (sh->line_len == 0) {
                    shell_printf(sh, "\n");
                    cmd_logout(sh);
                    prompt(sh);
                }
            } else if (c >= 0x20 && c < 0x7f && sh->line_len < SHELL_LINE_MAX) {
                sh->line[sh->line_len++] = c;
                write_out(sh, &c, 1);
            }
        }
    }
}
