// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The names of the Wi-Fi networks a scan saw, for Tab at an SSID question (net/wifi.c keeps them
// only while one is pending). Pure C, host tested.

#ifndef SOS_WIFI_NAMES_H
#define SOS_WIFI_NAMES_H

#include <stddef.h>
#include <stdint.h>

#define WIFI_NAMES_MAX     16      // networks remembered: the strongest
#define WIFI_NAME_LEN_MAX  32      // 802.11's limit on an SSID

typedef struct {
    char ssid[WIFI_NAME_LEN_MAX + 1];
    int16_t rssi;           // dBm, the strongest reading
} wifi_name_t;

// Add what a scan heard: `len` bytes of SSID at `ssid` (not NUL-terminated), at `rssi` dBm. The
// list (`*count` entries) keeps one entry per name with its strongest reading, strongest first;
// when it's full, a weaker name than all of it is dropped, a stronger one pushes out the
// weakest. Hidden networks (an empty name) and names holding a NUL byte are left out.
void wifi_names_add(wifi_name_t *list, unsigned *count, const uint8_t *ssid, size_t len,
                    int rssi);

#endif
