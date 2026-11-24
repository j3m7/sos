// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include <stdio.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "lwip/netif.h"

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/wc_port.h>
#include <wolfssh/ssh.h>

#include "hostkey.h"
#include "shell.h"
#include "stackmon.h"
#include "sos_config.h"
#include "sshd.h"
#include "wdog.h"

// The USB/UART console shell. stdio already translates "\n" to "\r\n".
static int console_read(void *ctx, char *buf, int len) {
    (void)ctx;
    int n = 0;
    int c;
    while (n < len && (c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT)
        buf[n++] = (char)c;
    return n;
}

static void console_write(void *ctx, const char *buf, int len) {
    (void)ctx;
    fwrite(buf, 1, (size_t)len, stdout);
    fflush(stdout);
}

static const shell_io_t console_io = {
    .name = "console",
    .read = console_read,
    .write = console_write,
    .logout = NULL,
    .crlf = false,
};

static shell_t console_shell;

// cyw43_arch_wifi_connect_timeout_ms(), but feeding the watchdog while it waits (a connect
// attempt takes longer than the watchdog timeout). Same logic as the SDK's version.
static int wifi_connect_once(uint32_t timeout_ms) {
    int err = cyw43_arch_wifi_connect_async(SOS_WIFI_SSID, SOS_WIFI_PASSWORD,
                                            CYW43_AUTH_WPA2_AES_PSK);
    if (err)
        return err;
    absolute_time_t until = make_timeout_time_ms(timeout_ms);
    for (;;) {
        int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (st == CYW43_LINK_UP)
            return PICO_OK;
        if (st == CYW43_LINK_BADAUTH)
            return PICO_ERROR_BADAUTH;
        if (st == CYW43_LINK_FAIL)
            return PICO_ERROR_CONNECT_FAILED;
        if (st == CYW43_LINK_NONET) {   // network not found yet: keep trying until the timeout
            err = cyw43_arch_wifi_connect_async(SOS_WIFI_SSID, SOS_WIFI_PASSWORD,
                                                CYW43_AUTH_WPA2_AES_PSK);
            if (err)
                return err;
        }
        if (time_reached(until))
            return PICO_ERROR_TIMEOUT;
        wdog_feed();
        sleep_ms(10);
    }
}

static bool wifi_connect(void) {
    cyw43_arch_enable_sta_mode();
    netif_set_hostname(&cyw43_state.netif[CYW43_ITF_STA], SOS_HOSTNAME);

    for (int attempt = 1; attempt <= 5; attempt++) {
        printf("wifi: connecting to '%s' (attempt %d)...\n", SOS_WIFI_SSID, attempt);
        int err = wifi_connect_once(30000);
        if (err == 0) {
            cyw43_arch_lwip_begin();
            printf("wifi: connected, ip %s\n",
                   ip4addr_ntoa(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA])));
            cyw43_arch_lwip_end();
            return true;
        }
        printf("wifi: connect failed (%d)\n", err);
    }
    return false;
}

int main(void) {
    stackmon_paint();
    stdio_init_all();

    // Give a USB serial console a moment to attach so boot messages aren't lost.
    for (int i = 0; i < 20 && !stdio_usb_connected(); i++)
        sleep_ms(100);
    printf("\nSOS booting\n");

    // Covers the rest of boot too: a hang there resets and retries instead of sitting dead.
    wdog_start();
    if (wdog_caused_reset())
        printf("last reset: watchdog timeout\n");

    if (wolfCrypt_Init() != 0 || wolfSSH_Init() != WS_SUCCESS) {
        printf("crypto init failed\n");
        return 1;
    }

    // Host key first: generating it may write flash, which is simplest before Wi-Fi runs.
    const uint8_t *hostkey;
    uint32_t hostkey_len;
    bool have_hostkey = hostkey_get(&hostkey, &hostkey_len);

    if (cyw43_arch_init()) {
        printf("cyw43 init failed\n");
        return 1;
    }

    if (wifi_connect() && have_hostkey)
        sshd_start(hostkey, hostkey_len);

    shell_init(&console_shell, &console_io, NULL);

    absolute_time_t next_blink = get_absolute_time();
    bool led = false;
    for (;;) {
        wdog_feed();
        shell_poll(&console_shell);
        sshd_poll();
        if (time_reached(next_blink)) {
            led = !led;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led);
            next_blink = make_timeout_time_ms(led ? 50 : 1950);   // brief blink every 2 s
        }
        sleep_ms(5);
    }
}
