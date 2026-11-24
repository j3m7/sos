// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "wifi.h"

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "lwip/netif.h"

#include "log.h"
#include "settings.h"

#define CONNECT_TIMEOUT_MS  30000   // one attempt
#define REJOIN_INTERVAL_MS  3000    // re-issue the join while the network isn't found
#define RETRY_DELAY_MS      15000   // after a failed attempt or a dropped link

typedef enum { W_OFF, W_CONNECTING, W_UP, W_WAIT } wifi_st_t;

static wifi_st_t state = W_OFF;
static absolute_time_t deadline;        // W_CONNECTING: give up; W_WAIT: retry
static absolute_time_t next_rejoin;
static unsigned attempt;

static struct netif *sta(void) {
    return &cyw43_state.netif[CYW43_ITF_STA];
}

static uint32_t auth_mode(void) {
    return settings()->wifi_password[0] ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN;
}

static void begin_attempt(void) {
    const sos_config_t *c = settings();
    if (!c->wifi_ssid[0]) {
        state = W_OFF;
        return;
    }
    attempt++;
    // The hostname is sent with DHCP requests; the string must stay valid (it's in settings).
    netif_set_hostname(sta(), c->hostname);
    log_msg(SOS_LOG_NOTICE, "wifi: connecting to '%s' (attempt %u)", c->wifi_ssid, attempt);
    int err = cyw43_arch_wifi_connect_async(c->wifi_ssid, c->wifi_password, auth_mode());
    if (err) {
        log_msg(SOS_LOG_WARNING, "wifi: connect request failed (%d)", err);
        state = W_WAIT;
        deadline = make_timeout_time_ms(RETRY_DELAY_MS);
        return;
    }
    state = W_CONNECTING;
    deadline = make_timeout_time_ms(CONNECT_TIMEOUT_MS);
    next_rejoin = make_timeout_time_ms(REJOIN_INTERVAL_MS);
}

static void retry_later(const char *why) {
    log_msg(SOS_LOG_WARNING, "wifi: %s; retrying in %d s", why, RETRY_DELAY_MS / 1000);
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    state = W_WAIT;
    deadline = make_timeout_time_ms(RETRY_DELAY_MS);
}

void wifi_start(void) {
    cyw43_arch_enable_sta_mode();
    attempt = 0;
    begin_attempt();
    if (state == W_OFF)
        log_msg(SOS_LOG_NOTICE, "wifi: no network configured (use 'setup' or 'wifi set' on the console)");
}

void wifi_reconnect(void) {
    if (state != W_OFF)
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    attempt = 0;
    begin_attempt();
}

void wifi_poll(void) {
    int link;
    switch (state) {
    case W_OFF:
        break;
    case W_CONNECTING:
        link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (link == CYW43_LINK_UP) {
            char ip[16];
            cyw43_arch_lwip_begin();
            ip4addr_ntoa_r(netif_ip4_addr(sta()), ip, sizeof(ip));
            cyw43_arch_lwip_end();
            log_msg(SOS_LOG_NOTICE, "wifi: connected, ip %s", ip);
            state = W_UP;
            attempt = 0;
        } else if (link == CYW43_LINK_BADAUTH) {
            retry_later("wrong password");
        } else if (link == CYW43_LINK_FAIL) {
            retry_later("connection failed");
        } else if (time_reached(deadline)) {
            retry_later("no connection within 30 s");
        } else if (link == CYW43_LINK_NONET && time_reached(next_rejoin)) {
            // Network not found (yet): the join has ended, so ask again until the deadline.
            cyw43_arch_wifi_connect_async(settings()->wifi_ssid, settings()->wifi_password,
                                          auth_mode());
            next_rejoin = make_timeout_time_ms(REJOIN_INTERVAL_MS);
        }
        break;
    case W_UP:
        link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (link != CYW43_LINK_UP) {
            log_msg(SOS_LOG_WARNING, "wifi: link lost (status %d), reconnecting", link);
            cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
            begin_attempt();
        }
        break;
    case W_WAIT:
        if (time_reached(deadline))
            begin_attempt();
        break;
    }
}

bool wifi_up(void) {
    return state == W_UP;
}

const char *wifi_state(void) {
    switch (state) {
    case W_OFF:        return settings()->wifi_ssid[0] ? "stopped" : "not configured";
    case W_CONNECTING: return "connecting";
    case W_UP:         return "connected";
    case W_WAIT:       return "waiting to retry";
    }
    return "?";
}
