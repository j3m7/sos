// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The names of the Wi-Fi networks a scan saw (see wifi_names.h).

#include "net/wifi_names.h"

#include <string.h>

void wifi_names_add(wifi_name_t *list, unsigned *count, const uint8_t *ssid, size_t len,
                    int rssi) {
    if (len == 0 || len > WIFI_NAME_LEN_MAX || memchr(ssid, 0, len))
        return;
    unsigned n = *count;
    unsigned i;
    for (i = 0; i < n; i++)
        if (!strncmp(list[i].ssid, (const char *)ssid, len) && list[i].ssid[len] == '\0')
            break;
    if (i < n) {                        // heard before: keep the stronger reading
        if (rssi <= list[i].rssi)
            return;
    } else if (n < WIFI_NAMES_MAX) {    // new, and there's room
        i = n++;
    } else {                            // new, list full: only if stronger than the weakest
        if (rssi <= list[n - 1].rssi)
            return;
        i = n - 1;
    }
    // Take the entry out, then put it back where its reading sorts it (strongest first).
    wifi_name_t e;
    memcpy(e.ssid, ssid, len);
    e.ssid[len] = '\0';
    e.rssi = (int16_t)rssi;
    while (i > 0 && list[i - 1].rssi < e.rssi) {
        list[i] = list[i - 1];
        i--;
    }
    list[i] = e;
    *count = n;
}
