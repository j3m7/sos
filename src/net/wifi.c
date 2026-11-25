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

// A network may have several access points (a mesh, or repeaters), and the device should be on
// the strongest. The chip doesn't see to that: joining by name, it took one at -66 dBm with
// another at -34 dBm in reach in 8 of 12 boots. So each attempt scans for the network first,
// then lets the chip join by name. Once connected, the access point it chose is compared with
// the scan; if another was stronger by SWITCH_MARGIN_DB, the device leaves and joins that one
// by address, which takes about 3 s more. A scan can miss an access point, so if it found
// fewer than two, a second scan runs before the comparison (W_VERIFY).
// Joining by address straight away isn't used: the first such join after boot failed every
// time (the retry worked), and with the scan's channel passed too it never completed.
#define SCAN_TIMEOUT_MS     10000   // a scan takes about 0.5 s
#define SCAN_MAX_APS        8       // access points of our network remembered per scan
#define SWITCH_MARGIN_DB    10      // readings of one access point vary by a few dB
#define SWITCH_TRIES        2       // moves to a stronger access point per connection

typedef enum { W_OFF, W_SCANNING, W_CONNECTING, W_VERIFY, W_UP, W_WAIT } wifi_st_t;

static wifi_st_t state = W_OFF;
static absolute_time_t deadline;        // W_CONNECTING: give up; W_WAIT: retry
static absolute_time_t next_rejoin;
static absolute_time_t badauth_until;   // W_CONNECTING: BADAUTH still showing then = wrong password
static bool badauth_seen;
static unsigned attempt;

// Scan results for our network. Written by scan_result() in the driver's context (interrupt),
// read by the main loop once the scan has ended.
static struct {
    uint8_t bssid[6];
    int16_t rssi;
} scan_aps[SCAN_MAX_APS];
static volatile unsigned scan_count;
static bool prefer_bssid;               // join the scan's strongest by address (see above)
static unsigned switch_tries;
static bool verified;                   // the second scan (W_VERIFY) has run this attempt
static bool join_bssid_set;             // this attempt joins join_bssid, not just the name
static uint8_t join_bssid[6];
static bool keepalive = true;
static absolute_time_t next_keepalive;

static struct netif *sta(void) {
    return &cyw43_state.netif[CYW43_ITF_STA];
}

static uint32_t auth_mode(void) {
    return settings()->wifi_password[0] ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN;
}

// A scan result: remember it if it's one of our network's access points (the strongest reading
// of each). Runs in the driver's context.
static int scan_result(void *env, const cyw43_ev_scan_result_t *r) {
    (void)env;
    const char *ssid = settings()->wifi_ssid;
    size_t len = strlen(ssid);
    if (!r || r->ssid_len != len || memcmp(r->ssid, ssid, len))
        return 0;
    unsigned i;
    for (i = 0; i < scan_count; i++)
        if (!memcmp(scan_aps[i].bssid, r->bssid, 6))
            break;
    if (i == SCAN_MAX_APS)
        return 0;
    if (i == scan_count) {
        memcpy(scan_aps[i].bssid, r->bssid, 6);
        scan_aps[i].rssi = r->rssi;
        scan_count = i + 1;
    } else if (r->rssi > scan_aps[i].rssi) {
        scan_aps[i].rssi = r->rssi;
    }
    return 0;
}

// Ask the chip to join: the access point chosen by the scan, or any with the network's name.
static int join(void) {
    const sos_config_t *c = settings();
    // No channel is passed: with the scan's channel (cyw43_wifi_join's last argument) the join
    // never completed on this chip firmware.
    if (join_bssid_set)
        return cyw43_arch_wifi_connect_bssid_async(c->wifi_ssid, join_bssid, c->wifi_password,
                                                   auth_mode());
    return cyw43_arch_wifi_connect_async(c->wifi_ssid, c->wifi_password, auth_mode());
}

static unsigned strongest(void) {
    unsigned best = 0;
    for (unsigned i = 1; i < scan_count; i++)
        if (scan_aps[i].rssi > scan_aps[best].rssi)
            best = i;
    return best;
}

static void start_join(void) {
    const sos_config_t *c = settings();
    join_bssid_set = prefer_bssid;      // move_to_strongest() chose join_bssid
    unsigned n = scan_count;
    if (n) {
        char list[SCAN_MAX_APS * 28 + 1];
        size_t used = 0;
        for (unsigned i = 0; i < n; i++) {
            const uint8_t *b = scan_aps[i].bssid;
            used += (size_t)snprintf(list + used, sizeof(list) - used,
                                     "%s%02x:%02x:%02x:%02x:%02x:%02x %d dBm", i ? ", " : "",
                                     b[0], b[1], b[2], b[3], b[4], b[5], (int)scan_aps[i].rssi);
        }
        log_msg(SOS_LOG_INFO, "wifi: access points of '%s': %s", c->wifi_ssid, list);
    } else if (!prefer_bssid) {
        // Not seen (out of reach, or a hidden network): the chip looks for it by name.
        log_msg(SOS_LOG_INFO, "wifi: '%s' not seen in the scan", c->wifi_ssid);
    }
    int err = join();
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
    scan_count = 0;
    verified = false;
    cyw43_wifi_scan_options_t opts = {0};
    if (cyw43_wifi_scan(&cyw43_state, &opts, NULL, scan_result) != 0) {
        start_join();                   // no scan: join by name
        return;
    }
    state = W_SCANNING;
    deadline = make_timeout_time_ms(SCAN_TIMEOUT_MS);
}

// Connected: if the scan saw a clearly stronger access point than the one the chip joined,
// move to it. True if it has started doing so.
static bool move_to_strongest(void) {
    uint8_t cur[6];
    bool moving = false;
    unsigned n = scan_count, best = strongest(), mine;
    if (n >= 2 && cyw43_wifi_get_bssid(&cyw43_state, cur) == 0) {
        for (mine = 0; mine < n; mine++)
            if (!memcmp(scan_aps[mine].bssid, cur, 6))
                break;
        // mine == n: joined one the scan missed, so there's nothing to compare.
        if (mine < n && mine != best && switch_tries < SWITCH_TRIES &&
            scan_aps[best].rssi - scan_aps[mine].rssi >= SWITCH_MARGIN_DB) {
            const uint8_t *b = scan_aps[best].bssid;
            log_msg(SOS_LOG_NOTICE, "wifi: joined %02x:%02x:%02x:%02x:%02x:%02x (%d dBm), but "
                    "%02x:%02x:%02x:%02x:%02x:%02x is stronger (%d dBm); moving to it",
                    cur[0], cur[1], cur[2], cur[3], cur[4], cur[5], (int)scan_aps[mine].rssi,
                    b[0], b[1], b[2], b[3], b[4], b[5], (int)scan_aps[best].rssi);
            switch_tries++;
            memcpy(join_bssid, b, 6);
            moving = true;
        }
    }
    prefer_bssid = moving;
    if (moving) {
        // Straight to the join: the target is known, and a new scan might miss it.
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        scan_count = 0;
        verified = true;
        start_join();
    }
    return moving;
}

static void connected(void) {
    char ip[16];
    cyw43_arch_lwip_begin();
    ip4addr_ntoa_r(netif_ip4_addr(sta()), ip, sizeof(ip));
    cyw43_arch_lwip_end();
    uint8_t b[6] = {0};
    int32_t rssi = 0;
    cyw43_wifi_get_bssid(&cyw43_state, b);
    cyw43_wifi_get_rssi(&cyw43_state, &rssi);
    log_msg(SOS_LOG_NOTICE, "wifi: connected, ip %s (access point %02x:%02x:%02x:%02x:%02x:%02x, "
            "%d dBm)", ip, b[0], b[1], b[2], b[3], b[4], b[5], (int)rssi);
    state = W_UP;
    attempt = 0;
    next_keepalive = make_timeout_time_ms(KEEPALIVE_MS);
}

static void retry_later(const char *why) {
    // Joining by address keeps failing: go back to joining by name.
    if (prefer_bssid && attempt > QUICK_RETRIES + 1)
        prefer_bssid = false;
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

uint32_t wifi_country_code(void) {
    const char *c = settings()->wifi_country;
    return c[0] ? CYW43_COUNTRY(c[0], c[1], 0) : CYW43_COUNTRY_WORLDWIDE;
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
    prefer_bssid = false;
    switch_tries = 0;
    begin_attempt();
}

void wifi_poll(void) {
    int link;
    switch (state) {
    case W_OFF:
        break;
    case W_SCANNING:
        if (!cyw43_wifi_scan_active(&cyw43_state) || time_reached(deadline))
            start_join();
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
            // A scan that found one access point may have missed another: look once more.
            cyw43_wifi_scan_options_t opts = {0};
            if (scan_count < 2 && !verified &&
                cyw43_wifi_scan(&cyw43_state, &opts, NULL, scan_result) == 0) {
                verified = true;
                state = W_VERIFY;
                deadline = make_timeout_time_ms(SCAN_TIMEOUT_MS);
            } else if (!move_to_strongest()) {
                connected();
            }
        } else if (link == CYW43_LINK_BADAUTH && time_reached(badauth_until)) {
            retry_later("wrong password");
        } else if (link == CYW43_LINK_FAIL) {
            retry_later("connection failed");
        } else if (time_reached(deadline)) {
            retry_later("no connection within 30 s");
        } else if (link == CYW43_LINK_NONET && time_reached(next_rejoin)) {
            // Network not found (yet): the join has ended, so ask again until the deadline.
            join();
            next_rejoin = make_timeout_time_ms(REJOIN_INTERVAL_MS);
        }
        break;
    case W_VERIFY:
        if (!cyw43_wifi_scan_active(&cyw43_state) || time_reached(deadline)) {
            state = W_CONNECTING;       // the link is checked again there
            if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP &&
                !move_to_strongest())
                connected();
        }
        break;
    case W_UP:
        link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (link != CYW43_LINK_UP) {
            log_msg(SOS_LOG_WARNING, "wifi: link lost (status %d), reconnecting", link);
            cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
            switch_tries = 0;
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
    case W_SCANNING:   return "scanning";
    case W_VERIFY:     return "connecting";
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
