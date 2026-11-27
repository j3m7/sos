// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "net/wifi.h"

#include "pico/cyw43_arch.h"
#include <string.h>

#include "pico/stdlib.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/stats.h"

#include "core/heaptag.h"
#include "core/log.h"
#include "core/settings.h"
#include "net/wifi_names.h"
#include "net/wifi_pick.h"

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

// Several networks may be saved (core/config.h); they are preferred in the order saved. Each
// attempt scans first and picks the network to join from what it saw (net/wifi_pick.h): the
// first saved network in sight that hasn't failed in this round, else one not in sight, by name
// (a hidden network, or out of reach). A network is given up on for the round after
// QUICK_RETRIES + 1 failed attempts, or, when there are others to try, at once on a wrong
// password or if the scan didn't see it. When every network has failed the round starts again
// after RETRY_DELAY_MS. With one network saved this is the same as before there were several.
//
// A network may also have several access points (a mesh, or repeaters), and the device should
// be on the strongest. The chip doesn't see to that: joining by name, it took one at -66 dBm
// with another at -34 dBm in reach in 8 of 12 boots. So the chip joins the chosen network by
// name, and once connected, the access point it chose is compared with the scan; if another of
// the same network was stronger by SWITCH_MARGIN_DB, the device leaves and joins that one by
// address, which takes about 3 s more. A scan can miss an access point, so if it found fewer
// than two of the network's, a second scan runs before the comparison (W_VERIFY).
// Joining by address straight away isn't used: the first such join after boot failed every
// time (the retry worked), and with the scan's channel passed too it never completed.
#define SCAN_TIMEOUT_MS     10000   // a scan takes about 0.5 s
#define SCAN_MAX_APS        12      // access points of our networks remembered per scan
#define SWITCH_MARGIN_DB    10      // readings of one access point vary by a few dB
#define SWITCH_TRIES        2       // moves to a stronger access point per connection
_Static_assert(CONFIG_WIFI_NETS <= WIFI_PICK_MAX_NETS, "one bit per network in failed");

typedef enum { W_OFF, W_SCANNING, W_CONNECTING, W_VERIFY, W_UP, W_WAIT } wifi_st_t;

static wifi_st_t state = W_OFF;
static absolute_time_t deadline;        // W_CONNECTING: give up; W_WAIT: retry
static absolute_time_t scan_deadline;   // W_SCANNING, W_VERIFY: stop waiting for the scan
static absolute_time_t next_rejoin;
static absolute_time_t badauth_until;   // W_CONNECTING: BADAUTH still showing then = wrong password
static bool badauth_seen;
static unsigned attempt;                // failed attempts since the last connection, + this one

// The saved networks: a copy of the configuration taken when an attempt begins. The scan
// callback runs in the driver's context (a low-priority interrupt with the threadsafe_background
// architecture), and the console edits the live configuration in place, so neither the callback
// nor a join in progress reads settings() directly. An edit takes effect at the next attempt;
// the console calls wifi_reconnect() after one.
static config_wifi_net_t nets[CONFIG_WIFI_NETS];
static unsigned net_count;
static unsigned cur;                    // the network of this attempt (index into nets)
static bool cur_seen;                   // ...and whether the scan saw it
static uint32_t failed;                 // networks given up on in this round, one bit each
static uint8_t tries[CONFIG_WIFI_NETS]; // failed attempts per network since the last connection
// The network whose DHCP address the interface holds (empty: none yet). The address stays on
// the interface after leaving, and the driver counts the link as up as soon as there's one, so
// joining another network with it would look connected at once, with an address that isn't
// that network's, until DHCP there refused it (and the link seemed lost). Seen on dev-1 going
// from a phone hotspot back to the home network.
static char addr_ssid[CONFIG_SSID_MAX + 1];

// Access points of our networks, as scanned. scan_rx is written by scan_result() in the
// driver's context; the main loop changes it, and copies it to scan_aps, only while holding the
// driver's lock (cyw43_arch_lwip_begin), so a scan still running (past its deadline) can't write
// it halfway through. The driver keeps one callback argument for all scans, not one per scan, so
// results arriving late from a scan started earlier can't be told apart; they are matched
// against the current networks all the same, and an access point heard is one in reach.
static wifi_ap_t scan_rx[SCAN_MAX_APS];
static unsigned scan_rx_count;
static wifi_ap_t scan_aps[SCAN_MAX_APS];    // main loop only: taken by take_scan()
static unsigned scan_count;
static bool prefer_bssid;               // join the scan's strongest by address (see above)
static unsigned switch_tries;
static bool verified;                   // the second scan (W_VERIFY) has run this attempt
static bool join_bssid_set;             // this attempt joins join_bssid, not just the name
static uint8_t join_bssid[6];
static bool keepalive = true;
static absolute_time_t next_keepalive;
static bool started;                    // wifi_start() has run: the chip can scan

// The names of all the networks a scan hears, for Tab at an SSID question (wifi_names_start()).
// Allocated only while one wants them, and freed NAMES_KEEP_MS after the last use if nobody
// says it's done (a question dropped without an answer). Written by scan_result() in the
// driver's context, so the main loop changes or reads it only under the driver's lock.
#define NAMES_KEEP_MS (10 * 60 * 1000)
static wifi_name_t *names;
static unsigned names_count;
static bool names_scanning;             // a scan that feeds names is running
static absolute_time_t names_until;

static struct netif *sta(void) {
    return &cyw43_state.netif[CYW43_ITF_STA];
}

static uint32_t auth_mode(void) {
    return nets[cur].password[0] ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN;
}

// A scan result: remember it if it's an access point of one of our networks (the strongest
// reading of each). Runs in the driver's context, which holds the driver's lock meanwhile; it
// reads only nets, which the main loop changes only under that lock (begin_attempt()).
static int scan_result(void *env, const cyw43_ev_scan_result_t *r) {
    (void)env;
    if (!r)
        return 0;
    if (names)                          // every network, for an SSID question's Tab
        wifi_names_add(names, &names_count, r->ssid, r->ssid_len, r->rssi);
    unsigned net;
    for (net = 0; net < net_count; net++)
        if (r->ssid_len == strlen(nets[net].ssid) && !memcmp(r->ssid, nets[net].ssid, r->ssid_len))
            break;
    if (net == net_count)
        return 0;
    unsigned i;
    for (i = 0; i < scan_rx_count; i++)
        if (!memcmp(scan_rx[i].bssid, r->bssid, 6))
            break;
    if (i == SCAN_MAX_APS)
        return 0;
    if (i == scan_rx_count) {
        memcpy(scan_rx[i].bssid, r->bssid, 6);
        scan_rx[i].rssi = r->rssi;
        scan_rx[i].net = (uint8_t)net;
        scan_rx_count = i + 1;
    } else if (r->rssi > scan_rx[i].rssi) {
        scan_rx[i].rssi = r->rssi;
    }
    return 0;
}

// Forget the scan results (before a new attempt's scan).
static void clear_scan(void) {
    cyw43_arch_lwip_begin();
    scan_rx_count = 0;
    cyw43_arch_lwip_end();
    scan_count = 0;
}

// Copy the results so far for the main loop to use.
static void take_scan(void) {
    cyw43_arch_lwip_begin();
    scan_count = scan_rx_count;
    memcpy(scan_aps, scan_rx, scan_count * sizeof(scan_aps[0]));
    cyw43_arch_lwip_end();
}

// How many access points of the current network the scan saw.
static unsigned cur_aps(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < scan_count; i++)
        n += scan_aps[i].net == cur;
    return n;
}

// Ask the chip to join: the access point chosen by the scan, or any with the network's name.
static int join(void) {
    const config_wifi_net_t *n = &nets[cur];
    // No channel is passed: with the scan's channel (cyw43_wifi_join's last argument) the join
    // never completed on this chip firmware.
    if (join_bssid_set)
        return cyw43_arch_wifi_connect_bssid_async(n->ssid, join_bssid, n->password, auth_mode());
    return cyw43_arch_wifi_connect_async(n->ssid, n->password, auth_mode());
}

// Log the access points the scan saw of the current network.
static void log_aps(void) {
    char list[SCAN_MAX_APS * 28 + 1];
    size_t used = 0;
    list[0] = '\0';
    for (unsigned i = 0; i < scan_count && used < sizeof(list) - 1; i++) {
        if (scan_aps[i].net != cur)
            continue;
        const uint8_t *b = scan_aps[i].bssid;
        int len = snprintf(list + used, sizeof(list) - used,
                           "%s%02x:%02x:%02x:%02x:%02x:%02x %d dBm", used ? ", " : "",
                           b[0], b[1], b[2], b[3], b[4], b[5], (int)scan_aps[i].rssi);
        // Cut short (an odd signal value takes more room): stop, don't run past the end.
        used = len < 0 || (size_t)len >= sizeof(list) - used ? sizeof(list) - 1
                                                             : used + (size_t)len;
    }
    log_msg(SOS_LOG_DAEMON | SOS_LOG_INFO, "wifi: access points of '%s': %s", nets[cur].ssid, list);
}

// Joining another network than the one the address is from: drop the address and the lease,
// so the link counts as up only once DHCP there has given one. (The same network again keeps
// it: DHCP asks to keep the address, and connections on it may survive.)
static void forget_address_unless_same_network(void) {
    if (!addr_ssid[0] || !strcmp(addr_ssid, nets[cur].ssid))
        return;
    cyw43_arch_lwip_begin();
    dhcp_release_and_stop(sta());       // removes the address; not associated, so nothing is sent
    dhcp_start(sta());                  // starts over (discover) once the link is up
    cyw43_arch_lwip_end();
    addr_ssid[0] = '\0';
}

static void start_join(void) {
    join_bssid_set = prefer_bssid;      // move_to_strongest() chose join_bssid, in nets[cur]
    if (!prefer_bssid) {
        int pick = wifi_pick(scan_aps, scan_count, net_count, failed);
        if (pick < 0) {                 // can't happen: retry_later() starts a new round
            failed = 0;
            pick = wifi_pick(scan_aps, scan_count, net_count, failed);
        }
        cur = (unsigned)pick;
        cur_seen = wifi_pick_seen(scan_aps, scan_count, cur);
        if (net_count > 1)
            log_msg(SOS_LOG_DAEMON | SOS_LOG_INFO,
                    "wifi: trying '%s' (saved network %u of %u)", nets[cur].ssid,
                    cur + 1, net_count);
        if (cur_seen)
            log_aps();
        else            // not seen (out of reach, or a hidden network): the chip looks for it by name
            log_msg(SOS_LOG_DAEMON | SOS_LOG_INFO,
                    "wifi: '%s' not seen in the scan", nets[cur].ssid);
        forget_address_unless_same_network();
    }
    int err = join();
    if (err) {
        log_msg(SOS_LOG_DAEMON | SOS_LOG_WARNING, "wifi: connect request failed (%d)", err);
        state = W_WAIT;
        deadline = make_timeout_time_ms(RETRY_DELAY_MS);
        return;
    }
    state = W_CONNECTING;
    deadline = make_timeout_time_ms(CONNECT_TIMEOUT_MS);
    next_rejoin = make_timeout_time_ms(REJOIN_INTERVAL_MS);
    badauth_seen = false;
}

// Forget which networks failed: after a connection, or when the saved networks change.
static void reset_failures(void) {
    failed = 0;
    memset(tries, 0, sizeof(tries));
}

static void begin_attempt(void) {
    const sos_config_t *c = settings();
    unsigned count = config_wifi_count(c);
    if (!count) {
        state = W_OFF;
        return;
    }
    attempt++;
    static config_wifi_net_t now[CONFIG_WIFI_NETS];     // 485 bytes: not on the stack
    memset(now, 0, sizeof(now));
    for (unsigned i = 0; i < count; i++)
        config_wifi_get(c, i, &now[i]);
    // A scan past its deadline may still be running, so under the driver's lock.
    cyw43_arch_lwip_begin();
    bool changed = count != net_count || memcmp(now, nets, sizeof(nets));
    memcpy(nets, now, sizeof(nets));
    net_count = count;
    cyw43_arch_lwip_end();
    memset(now, 0, sizeof(now));
    if (changed) {                      // the failures counted were for other networks
        reset_failures();
        prefer_bssid = false;
    }
    // The hostname is sent with DHCP requests; the string must stay valid (it's in settings).
    netif_set_hostname(sta(), c->hostname);
    if (net_count == 1)
        log_msg(SOS_LOG_DAEMON | SOS_LOG_NOTICE,
                "wifi: connecting to '%s' (attempt %u)", nets[0].ssid, attempt);
    else
        log_msg(SOS_LOG_DAEMON | SOS_LOG_NOTICE,
                "wifi: looking for %u saved networks (attempt %u)", net_count,
                attempt);
    clear_scan();
    verified = false;
    cyw43_wifi_scan_options_t opts = {0};
    if (cyw43_wifi_scan(&cyw43_state, &opts, NULL, scan_result) != 0) {
        start_join();                   // no scan: join by name
        return;
    }
    state = W_SCANNING;
    scan_deadline = make_timeout_time_ms(SCAN_TIMEOUT_MS);
}

// Connected: if the scan saw a clearly stronger access point of this network than the one the
// chip joined, move to it. True if it has started doing so.
static bool move_to_strongest(void) {
    uint8_t now[6];
    bool moving = false;
    int best = wifi_pick_strongest(scan_aps, scan_count, cur);
    unsigned mine;
    if (cur_aps() >= 2 && cyw43_wifi_get_bssid(&cyw43_state, now) == 0) {
        for (mine = 0; mine < scan_count; mine++)
            if (scan_aps[mine].net == cur && !memcmp(scan_aps[mine].bssid, now, 6))
                break;
        // mine == scan_count: joined one the scan missed, so there's nothing to compare.
        if (mine < scan_count && (int)mine != best && switch_tries < SWITCH_TRIES &&
            scan_aps[best].rssi - scan_aps[mine].rssi >= SWITCH_MARGIN_DB) {
            const uint8_t *b = scan_aps[best].bssid;
            log_msg(SOS_LOG_DAEMON | SOS_LOG_NOTICE,
                    "wifi: joined %02x:%02x:%02x:%02x:%02x:%02x (%d dBm), but "
                    "%02x:%02x:%02x:%02x:%02x:%02x is stronger (%d dBm); moving to it",
                    now[0], now[1], now[2], now[3], now[4], now[5], (int)scan_aps[mine].rssi,
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
        clear_scan();
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
    log_msg(SOS_LOG_DAEMON | SOS_LOG_NOTICE, "wifi: connected to '%s', ip %s (access point "
            "%02x:%02x:%02x:%02x:%02x:%02x, %d dBm)", nets[cur].ssid, ip, b[0], b[1], b[2], b[3],
            b[4], b[5], (int)rssi);
    state = W_UP;
    attempt = 0;
    reset_failures();
    strcpy(addr_ssid, nets[cur].ssid);
    next_keepalive = make_timeout_time_ms(KEEPALIVE_MS);
}

// The attempt failed. wrong_password: the access point said so (not a timeout).
static void retry_later(const char *why, bool wrong_password) {
    if (tries[cur] < UINT8_MAX)
        tries[cur]++;
    // Give the network up for this round? With others to try, a wrong password or a network
    // not in sight is not worth retrying straight away.
    bool give_up = tries[cur] > QUICK_RETRIES ||
                   (net_count > 1 && (wrong_password || !cur_seen));
    if (give_up)
        failed |= 1u << cur;
    bool round_over = failed == (1u << net_count) - 1;
    if (round_over)
        failed = 0;
    // Joining by address keeps failing, or the network is given up for another: back to
    // joining by name.
    if (prefer_bssid && (attempt > QUICK_RETRIES + 1 || (net_count > 1 && give_up)))
        prefer_bssid = false;
    // The first failures are often transient (see QUICK_RETRIES), so retry those soon, and
    // move on to the next network soon too. Wait longer once every network has failed.
    int delay_ms = round_over ? RETRY_DELAY_MS : QUICK_RETRY_DELAY_MS;
    if (net_count > 1 && give_up && !round_over)
        log_msg(SOS_LOG_DAEMON | SOS_LOG_WARNING, "wifi: '%s': %s; trying the next network in %d s",
                nets[cur].ssid, why, delay_ms / 1000);
    else
        log_msg(SOS_LOG_DAEMON | SOS_LOG_WARNING,
                "wifi: %s; retrying in %d s", why, delay_ms / 1000);
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
        log_msg(SOS_LOG_DAEMON | SOS_LOG_WARNING,
                "wifi: setting power saving %s failed (%d)", on ? "on" : "off", err);
}

uint32_t wifi_country_code(void) {
    const char *c = settings()->wifi_country;
    return c[0] ? CYW43_COUNTRY(c[0], c[1], 0) : CYW43_COUNTRY_WORLDWIDE;
}

// The driver doesn't check that the chip took the country: one it doesn't know (UK for GB, say)
// is ignored without a word, and the chip stays on its own default. Say so.
static void check_country(void) {
    const char *want = settings()->wifi_country;
    uint32_t got = 0;
    if (!want[0] || wifi_iovar_get("country", &got) != 0)
        return;
    if ((got & 0xffff) != (wifi_country_code() & 0xffff))
        log_msg(SOS_LOG_DAEMON | SOS_LOG_WARNING,
                "wifi: the chip doesn't know country %s and uses %c%c; "
                "change it with 'wifi country'", want,
                (got & 0x7f) >= 0x20 ? (int)(got & 0x7f) : '?',
                (got >> 8 & 0x7f) >= 0x20 ? (int)(got >> 8 & 0x7f) : '?');
}

void wifi_start(void) {
    cyw43_arch_enable_sta_mode();   // sets the SDK's default power mode
    started = true;
    check_country();
    wifi_apply_power();
    attempt = 0;
    begin_attempt();
    if (state == W_OFF)
        log_msg(SOS_LOG_DAEMON | SOS_LOG_NOTICE,
                "wifi: no network configured (use 'setup' or 'wifi set' on the console)");
}

void wifi_reconnect(void) {
    if (state != W_OFF)
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    attempt = 0;
    prefer_bssid = false;
    switch_tries = 0;
    reset_failures();               // every network gets its chance again, the first one first
    begin_attempt();
}

void wifi_names_start(void) {
    if (!started)
        return;
    if (!names) {
        wifi_name_t *n = heap_zalloc(HEAP_WIFI, WIFI_NAMES_MAX * sizeof(*n));
        if (!n)
            return;
        cyw43_arch_lwip_begin();
        names = n;
        names_count = 0;
        cyw43_arch_lwip_end();
    }
    names_until = make_timeout_time_ms(NAMES_KEEP_MS);
    // A scan of the connection's own already running feeds the names too (one callback for
    // all scans). Ours running makes a connection attempt meanwhile join by name, unscanned.
    cyw43_wifi_scan_options_t opts = {0};
    names_scanning = cyw43_wifi_scan_active(&cyw43_state) ||
                     cyw43_wifi_scan(&cyw43_state, &opts, NULL, scan_result) == 0;
}

bool wifi_names_each(void (*fn)(const char *ssid, int rssi, void *arg), void *arg) {
    wifi_name_t copy[WIFI_NAMES_MAX];   // 576 bytes: fn may take its time, and print
    unsigned n = 0;
    cyw43_arch_lwip_begin();
    if (names) {
        n = names_count;
        memcpy(copy, names, n * sizeof(copy[0]));
    }
    cyw43_arch_lwip_end();
    if (!names)
        return false;
    names_until = make_timeout_time_ms(NAMES_KEEP_MS);
    for (unsigned i = 0; i < n; i++)
        fn(copy[i].ssid, copy[i].rssi, arg);
    return names_scanning && cyw43_wifi_scan_active(&cyw43_state);
}

void wifi_names_stop(void) {
    cyw43_arch_lwip_begin();
    wifi_name_t *n = names;
    names = NULL;
    names_count = 0;
    cyw43_arch_lwip_end();
    names_scanning = false;
    heap_release(HEAP_WIFI, n);
}

void wifi_poll(void) {
    int link;
    if (names_scanning && !cyw43_wifi_scan_active(&cyw43_state))
        names_scanning = false;
    if (names && time_reached(names_until))
        wifi_names_stop();
    switch (state) {
    case W_OFF:
        break;
    case W_SCANNING:
        if (!cyw43_wifi_scan_active(&cyw43_state) || time_reached(scan_deadline)) {
            take_scan();
            start_join();
        }
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
            if (cur_aps() < 2 && !verified &&
                cyw43_wifi_scan(&cyw43_state, &opts, NULL, scan_result) == 0) {
                verified = true;
                state = W_VERIFY;       // the attempt's own deadline stays as it is
                scan_deadline = make_timeout_time_ms(SCAN_TIMEOUT_MS);
            } else if (!move_to_strongest()) {
                connected();
            }
        } else if (link == CYW43_LINK_BADAUTH && time_reached(badauth_until)) {
            retry_later("wrong password", true);
        } else if (link == CYW43_LINK_FAIL) {
            retry_later("connection failed", false);
        } else if (time_reached(deadline)) {
            retry_later("no connection within 30 s", false);
        } else if (link == CYW43_LINK_NONET && time_reached(next_rejoin)) {
            // Network not found (yet): the join has ended, so ask again until the deadline.
            join();
            next_rejoin = make_timeout_time_ms(REJOIN_INTERVAL_MS);
        }
        break;
    case W_VERIFY:
        if (!cyw43_wifi_scan_active(&cyw43_state) || time_reached(scan_deadline)) {
            take_scan();
            state = W_CONNECTING;       // the link is checked again there
            if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP &&
                !move_to_strongest())
                connected();
        }
        break;
    case W_UP:
        link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (link != CYW43_LINK_UP) {
            log_msg(SOS_LOG_DAEMON | SOS_LOG_WARNING,
                    "wifi: link lost (status %d), reconnecting", link);
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

const char *wifi_ssid(void) {
    return state == W_UP ? nets[cur].ssid : NULL;
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

unsigned wifi_chip_counters(uint32_t out[WIFI_CHIP_COUNTER_WORDS]) {
    const size_t bytes = WIFI_CHIP_COUNTER_WORDS * sizeof(uint32_t);
    memset(out, 0, bytes);
    strcpy((char *)out, "counters");            // the request goes in the buffer the answer uses
    if (cyw43_ioctl(&cyw43_state, CYW43_IOCTL_GET_VAR, bytes, (uint8_t *)out, CYW43_ITF_STA) != 0)
        return 0;
    return WIFI_CHIP_COUNTER_WORDS;
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
