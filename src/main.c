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

static bool wifi_connect(void) {
    cyw43_arch_enable_sta_mode();
    netif_set_hostname(&cyw43_state.netif[CYW43_ITF_STA], SOS_HOSTNAME);

    for (int attempt = 1; attempt <= 5; attempt++) {
        printf("wifi: connecting to '%s' (attempt %d)...\n", SOS_WIFI_SSID, attempt);
        int err = cyw43_arch_wifi_connect_timeout_ms(SOS_WIFI_SSID, SOS_WIFI_PASSWORD,
                                                     CYW43_AUTH_WPA2_AES_PSK, 30000);
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

    shell_init();

    absolute_time_t next_blink = get_absolute_time();
    bool led = false;
    for (;;) {
        shell_poll();
        if (time_reached(next_blink)) {
            led = !led;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led);
            next_blink = make_timeout_time_ms(led ? 50 : 1950);   // brief blink every 2 s
        }
        sleep_ms(5);
    }
}
