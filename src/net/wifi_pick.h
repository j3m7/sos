// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Which saved Wi-Fi network to try next, from what a scan saw (net/wifi.c drives the radio).
//
// The networks are numbered in the order they are preferred. An attempt goes to the first one
// the scan saw that hasn't failed yet in this round; when none is left, to the first not yet
// failed that the scan didn't see (a hidden network, or one out of reach: the chip looks for it
// by name). So a hidden network is tried only when no other saved network is in sight. Once
// every network has failed, a new round starts. Within a network the strongest access point is
// preferred.
//
// Pure C: no Pico SDK dependency.

#ifndef SOS_WIFI_PICK_H
#define SOS_WIFI_PICK_H

#include <stdbool.h>
#include <stdint.h>

#define WIFI_PICK_MAX_NETS 32

// An access point a scan saw.
typedef struct {
    uint8_t bssid[6];
    int16_t rssi;           // dBm, the strongest reading
    uint8_t net;            // which saved network it belongs to
} wifi_ap_t;

// The network to try: as above, skipping those whose bit is set in failed. -1 if every one of
// the net_count networks has failed (or there are none).
int wifi_pick(const wifi_ap_t *aps, unsigned n, unsigned net_count, uint32_t failed);

// True if the scan saw network net.
bool wifi_pick_seen(const wifi_ap_t *aps, unsigned n, unsigned net);

// The index (into aps) of network net's strongest access point, or -1 if the scan saw none.
int wifi_pick_strongest(const wifi_ap_t *aps, unsigned n, unsigned net);

#endif
