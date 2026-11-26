// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/net/wifi_pick.c: which saved Wi-Fi network to try.

#include "net/wifi_pick.h"
#include "test.h"

static wifi_ap_t ap(uint8_t id, int rssi, uint8_t net) {
    wifi_ap_t a = { { 2, 0, 0, 0, 0, id }, (int16_t)rssi, net };
    return a;
}

TEST(no_networks) {
    CHECK_EQ(wifi_pick(NULL, 0, 0, 0), -1);
}

// One saved network: always that one, seen or not, until it has failed.
TEST(one_network) {
    wifi_ap_t aps[] = { ap(1, -60, 0) };
    CHECK_EQ(wifi_pick(aps, 1, 1, 0), 0);
    CHECK_EQ(wifi_pick(NULL, 0, 1, 0), 0);          // not seen: by name
    CHECK_EQ(wifi_pick(aps, 1, 1, 1), -1);
}

// The first in saved order that the scan saw, whatever the signal.
TEST(saved_order_wins_over_signal) {
    wifi_ap_t aps[] = { ap(1, -80, 2), ap(2, -30, 1), ap(3, -85, 1) };
    CHECK_EQ(wifi_pick(aps, 3, 3, 0), 1);           // 0 not seen; 1 before 2
    CHECK_EQ(wifi_pick(aps, 3, 3, 1u << 1), 2);     // 1 failed: on to 2
}

// Networks not seen are tried, in order, only when no seen one is left.
TEST(unseen_networks_after_seen_ones) {
    wifi_ap_t aps[] = { ap(1, -50, 2) };
    CHECK_EQ(wifi_pick(aps, 1, 4, 0), 2);
    CHECK_EQ(wifi_pick(aps, 1, 4, 1u << 2), 0);
    CHECK_EQ(wifi_pick(aps, 1, 4, 1u << 2 | 1u << 0), 1);
    CHECK_EQ(wifi_pick(aps, 1, 4, 1u << 2 | 1u << 0 | 1u << 1), 3);
    CHECK_EQ(wifi_pick(aps, 1, 4, 0xf), -1);
    CHECK_EQ(wifi_pick(NULL, 0, 4, 0), 0);          // nothing seen: in order
}

TEST(strongest_within_a_network) {
    wifi_ap_t aps[] = { ap(1, -70, 0), ap(2, -40, 1), ap(3, -55, 0), ap(4, -90, 0) };
    CHECK_EQ(wifi_pick_strongest(aps, 4, 0), 2);
    CHECK_EQ(wifi_pick_strongest(aps, 4, 1), 1);
    CHECK_EQ(wifi_pick_strongest(aps, 4, 2), -1);
    CHECK(wifi_pick_seen(aps, 4, 1));
    CHECK(!wifi_pick_seen(aps, 4, 3));
    CHECK_EQ(wifi_pick_strongest(aps, 0, 0), -1);
}

// More networks than bits in the mask: the rest are never picked rather than misread.
TEST(net_count_is_bounded) {
    CHECK_EQ(wifi_pick(NULL, 0, 100, 0xffffffffu), -1);
}

TEST_MAIN(
    RUN(no_networks);
    RUN(one_network);
    RUN(saved_order_wins_over_signal);
    RUN(unseen_networks_after_seen_ones);
    RUN(strongest_within_a_network);
    RUN(net_count_is_bounded);
)
