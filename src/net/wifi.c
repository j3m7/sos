// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "net/wifi.h"

#include "pico/cyw43_arch.h"
#include <string.h>

#include "pico/stdlib.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/stats.h"

#include "core/log.h"
#include "core/settings.h"

#define CONNECT_TIMEOUT_MS  30000   // one attempt
#define REJOIN_INTERVAL_MS  3000    // re-issue the join while the network isn't found
#define RETRY_DELAY_MS      15000   // after a failed attempt or a dropped link
// Attempts that are retried after QUICK_RETRY_DELAY_MS instead. Some access points leave
// authentication frames unanswered now and then; when the firmware's own retry times out too,
// the join fails (SET_SSID status 1, CYW43_LINK_FAIL) but an immediate new attempt usually works.
#define QUICK_RETRIES       2
#define QUICK_RETRY_DELAY_MS 1000
// How long CYW43_LINK_BADAUTH must last before it counts as a wrong password. The driver also
// reports it briefly when an authentication frame times out (AUTH event status 2) and clears it
// when the retry succeeds ~10 ms later; some access points do this on every join. Giving up on
// the first sighting made joins fail at random, depending on whether a poll landed in that window.
#define BADAUTH_GRACE_MS    3000
// Announce our address this often while connected (see wifi.h). A device that sends nothing
// stopped receiving broadcasts within a couple of minutes (with power saving on or off), and
// ~20 minutes later the router's ARP entry expired and nobody could reach it. Sending once a
// minute kept broadcasts arriving. One broadcast frame each time.
#define KEEPALIVE_MS        60000

// Broadcom ioctls the driver has no names for (WLC_GET_RATE 12, WLC_GET_PHY_NOISE 135), in its
// encoding: number << 1, bit 0 set for a write.
#define IOCTL_GET_RATE      (12 << 1)
#define IOCTL_GET_PHY_NOISE (135 << 1)

typedef enum { W_OFF, W_CONNECTING, W_UP, W_WAIT } wifi_st_t;

static wifi_st_t state = W_OFF;
static absolute_time_t deadline;        // W_CONNECTING: give up; W_WAIT: retry
static absolute_time_t next_rejoin;
static absolute_time_t badauth_until;   // W_CONNECTING: BADAUTH still showing then = wrong password
static bool badauth_seen;
static unsigned attempt;
static bool keepalive = true;
static absolute_time_t next_keepalive;

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
    badauth_seen = false;
}

static void retry_later(const char *why) {
    // The first failures are often transient (see QUICK_RETRIES), so retry those soon.
    int delay_ms = attempt <= QUICK_RETRIES ? QUICK_RETRY_DELAY_MS : RETRY_DELAY_MS;
    log_msg(SOS_LOG_WARNING, "wifi: %s; retrying in %d s", why, delay_ms / 1000);
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    state = W_WAIT;
    deadline = make_timeout_time_ms(delay_ms);
}

void wifi_apply_power(void) {
    // On: the SDK's default (PM2: the radio sleeps between beacons after 200 ms without traffic).
    // Off: the radio stays awake.
    bool on = settings()->wifi_powersave;
    int err = cyw43_wifi_pm(&cyw43_state, on ? CYW43_PERFORMANCE_PM : CYW43_NONE_PM);
    if (err)
        log_msg(SOS_LOG_WARNING, "wifi: setting power saving %s failed (%d)", on ? "on" : "off", err);
}

void wifi_start(void) {
    cyw43_arch_enable_sta_mode();   // sets the SDK's default power mode
    wifi_apply_power();
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
        if (link != CYW43_LINK_BADAUTH)
            badauth_seen = false;
        else if (!badauth_seen) {
            badauth_seen = true;
            badauth_until = make_timeout_time_ms(BADAUTH_GRACE_MS);
        }
        if (link == CYW43_LINK_UP) {
            char ip[16];
            cyw43_arch_lwip_begin();
            ip4addr_ntoa_r(netif_ip4_addr(sta()), ip, sizeof(ip));
            cyw43_arch_lwip_end();
            log_msg(SOS_LOG_NOTICE, "wifi: connected, ip %s", ip);
            state = W_UP;
            attempt = 0;
            next_keepalive = make_timeout_time_ms(KEEPALIVE_MS);
        } else if (link == CYW43_LINK_BADAUTH && time_reached(badauth_until)) {
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
        } else if (keepalive && time_reached(next_keepalive)) {
            cyw43_arch_lwip_begin();
            etharp_gratuitous(sta());
            cyw43_arch_lwip_end();
            next_keepalive = make_timeout_time_ms(KEEPALIVE_MS);
        }
        break;
    case W_WAIT:
        if (time_reached(deadline))
            begin_attempt();
        break;
    }
}

void wifi_set_keepalive(bool on) {
    keepalive = on;
    next_keepalive = get_absolute_time();       // announce straight away when turned on
}

bool wifi_keepalive(void) {
    return keepalive;
}

void wifi_arp_counts(unsigned *received, unsigned *sent) {
    cyw43_arch_lwip_begin();
    *received = lwip_stats.etharp.recv;
    *sent = lwip_stats.etharp.xmit;
    cyw43_arch_lwip_end();
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

bool wifi_radio(wifi_radio_t *r) {
    memset(r, 0, sizeof(*r));
    if (state != W_UP)
        return false;
    uint32_t ch[3] = {0};           // channel_info_t: hardware, target, scan channel
    // Extras; left 0 if the chip doesn't answer.
    cyw43_ioctl(&cyw43_state, IOCTL_GET_RATE, sizeof(r->rate), (uint8_t *)&r->rate, CYW43_ITF_STA);
    cyw43_ioctl(&cyw43_state, IOCTL_GET_PHY_NOISE, sizeof(r->noise), (uint8_t *)&r->noise,
                CYW43_ITF_STA);
    uint8_t var[12] = "qtxpower";
    if (cyw43_ioctl(&cyw43_state, CYW43_IOCTL_GET_VAR, sizeof(var), var, CYW43_ITF_STA) == 0) {
        memcpy(&r->txpower, var, 4);
        r->txpower &= 0xff;         // the top bit flags an override
    }
    return cyw43_wifi_get_rssi(&cyw43_state, &r->rssi) == 0 &&
           cyw43_wifi_get_bssid(&cyw43_state, r->bssid) == 0 &&
           cyw43_ioctl(&cyw43_state, CYW43_IOCTL_GET_CHANNEL, sizeof(ch), (uint8_t *)ch,
                       CYW43_ITF_STA) == 0 &&
           ((r->channel = ch[0]), true);
}

unsigned wifi_chip_counters(uint32_t *out, unsigned max) {
    static uint8_t buf[1024];       // too big for the stack
    memset(buf, 0, sizeof(buf));
    strcpy((char *)buf, "counters");
    if (cyw43_ioctl(&cyw43_state, CYW43_IOCTL_GET_VAR, sizeof(buf), buf, CYW43_ITF_STA) != 0)
        return 0;
    unsigned n = sizeof(buf) / 4;
    if (n > max)
        n = max;
    memcpy(out, buf, n * 4);
    return n;
}

int wifi_iovar_get(const char *name, uint32_t *value) {
    uint8_t buf[64] = {0};
    size_t n = strlen(name);
    if (n + 1 > sizeof(buf) - 4)
        return -1;
    memcpy(buf, name, n + 1);
    int r = cyw43_ioctl(&cyw43_state, CYW43_IOCTL_GET_VAR, sizeof(buf), buf, CYW43_ITF_STA);
    memcpy(value, buf, 4);
    return r;
}

int wifi_iovar_set(const char *name, uint32_t value) {
    uint8_t buf[64] = {0};
    size_t n = strlen(name);
    if (n + 1 > sizeof(buf) - 4)
        return -1;
    memcpy(buf, name, n + 1);
    memcpy(buf + n + 1, &value, 4);
    return cyw43_ioctl(&cyw43_state, CYW43_IOCTL_SET_VAR, n + 1 + 4, buf, CYW43_ITF_STA);
}
