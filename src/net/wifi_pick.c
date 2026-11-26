// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "net/wifi_pick.h"

bool wifi_pick_seen(const wifi_ap_t *aps, unsigned n, unsigned net) {
    return wifi_pick_strongest(aps, n, net) >= 0;
}

int wifi_pick_strongest(const wifi_ap_t *aps, unsigned n, unsigned net) {
    int best = -1;
    for (unsigned i = 0; i < n; i++)
        if (aps[i].net == net && (best < 0 || aps[i].rssi > aps[best].rssi))
            best = (int)i;
    return best;
}

int wifi_pick(const wifi_ap_t *aps, unsigned n, unsigned net_count, uint32_t failed) {
    if (net_count > WIFI_PICK_MAX_NETS)
        net_count = WIFI_PICK_MAX_NETS;
    for (unsigned i = 0; i < net_count; i++)
        if (!(failed >> i & 1) && wifi_pick_seen(aps, n, i))
            return (int)i;
    for (unsigned i = 0; i < net_count; i++)
        if (!(failed >> i & 1))
            return (int)i;
    return -1;
}
