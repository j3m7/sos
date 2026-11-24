// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Wi-Fi station connection, driven from the main loop without blocking: connects with the
// network in the live configuration, retries after failures and reconnects if the link drops.
// Power saving follows the configuration too (off by default).
//
// While connected it also announces its address once a minute (a gratuitous ARP). A device that
// sends nothing stops receiving broadcasts (seen within a couple of minutes, power saving on or
// off), so it can't answer ARP queries, and once the router's ARP entry expires it becomes
// unreachable ("no route to host") until it sends something itself.

#ifndef SOS_WIFI_H
#define SOS_WIFI_H

#include <stdbool.h>

// Enable station mode and start connecting if a network is configured. After cyw43_arch_init().
void wifi_start(void);

// Advance the connection: call from the main loop. Logs state changes.
void wifi_poll(void);

// Drop the current connection and connect again with the (possibly changed) configuration.
void wifi_reconnect(void);

// Apply the power-saving setting (settings()->wifi_powersave) to the radio: takes effect at once.
void wifi_apply_power(void);

// The keepalive announcement (on at boot). Off is only for testing: the device can then become
// unreachable after a while without traffic.
void wifi_set_keepalive(bool on);
bool wifi_keepalive(void);

// ARP packets received and sent since boot (lwIP's counters; they wrap at 65536).
void wifi_arp_counts(unsigned *received, unsigned *sent);

// True while associated and holding an IP address.
bool wifi_up(void);

// Short description of the state, e.g. "connected", "connecting", "not configured".
const char *wifi_state(void);

#endif
