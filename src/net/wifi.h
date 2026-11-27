// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Wi-Fi station connection, driven from the main loop without blocking: connects to one of the
// networks saved in the live configuration (the first in sight, in the order saved), retries
// after failures and reconnects if the link drops.
// Power saving follows the configuration too (off by default).
//
// While connected it also announces its address once a minute (a gratuitous ARP). A device that
// sends nothing stops receiving broadcasts (seen within a couple of minutes, power saving on or
// off), so it can't answer ARP queries, and once the router's ARP entry expires it becomes
// unreachable ("no route to host") until it sends something itself.

#ifndef SOS_WIFI_H
#define SOS_WIFI_H

#include <stdbool.h>
#include <stdint.h>

// The saved country as the driver's code, for cyw43_arch_init_with_country().
uint32_t wifi_country_code(void);

// Enable station mode and start connecting if a network is configured. After cyw43_arch_init().
void wifi_start(void);

// Advance the connection: call from the main loop. Logs state changes.
void wifi_poll(void);

// Drop the current connection and connect again with the (possibly changed) configuration,
// starting from the first saved network. While connected, a network preferred over the one in
// use that comes into reach isn't noticed: only a new attempt (this, or a dropped link) looks.
void wifi_reconnect(void);

// Apply the power-saving setting (settings()->wifi_powersave) to the radio: takes effect at once.
void wifi_apply_power(void);

// The keepalive announcement (on at boot). Off is only for testing: the device can then become
// unreachable after a while without traffic.
void wifi_set_keepalive(bool on);
bool wifi_keepalive(void);

// ARP packets received and sent since boot (lwIP's counters; they wrap at 65536).
void wifi_arp_counts(unsigned *received, unsigned *sent);

// Radio state while connected, from the Wi-Fi chip. False if a value couldn't be read.
typedef struct {
    int32_t rssi;           // dBm
    uint32_t channel;
    uint8_t bssid[6];       // the access point
    int32_t noise;          // dBm, the chip's noise floor estimate (0 if unknown)
    int32_t rate;           // current transmit rate, in 500 kbit/s units (0 if unknown)
    int32_t txpower;        // limit in quarter dBm; 127 = none set (0 if unknown)
} wifi_radio_t;
bool wifi_radio(wifi_radio_t *r);

// The chip's own counters ("counters" iovar): a version/length word, then 32-bit counters.
// Read straight into `out`, which the caller provides: WIFI_CHIP_COUNTER_WORDS words (the chip
// answers into a buffer of that size). Returns how many words it holds: that many, or 0 if the
// counters couldn't be read.
#define WIFI_CHIP_COUNTER_WORDS 256
unsigned wifi_chip_counters(uint32_t out[WIFI_CHIP_COUNTER_WORDS]);

// Debugging: read or write a 32-bit chip variable ("iovar") by name, e.g. "2g_rate", "ampdu_tx",
// "qtxpower". Returns the driver's result (0 = ok). The chip answers an unknown name with
// result 0 and the name's own bytes as the value. Changes last until the chip is restarted.
int wifi_iovar_get(const char *name, uint32_t *value);
int wifi_iovar_set(const char *name, uint32_t value);

// True while associated and holding an IP address.
bool wifi_up(void);

// The SSID of the network connected to, or NULL while not connected.
const char *wifi_ssid(void);

// Short description of the state, e.g. "connected", "connecting", "not configured".
const char *wifi_state(void);

#endif
