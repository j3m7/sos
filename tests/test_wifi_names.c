// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/net/wifi_names.c: the network names a scan saw, for Tab.

#include "net/wifi_names.h"
#include "test.h"

static wifi_name_t list[WIFI_NAMES_MAX];
static unsigned count;

static void add(const char *ssid, int rssi) {
    wifi_names_add(list, &count, (const uint8_t *)ssid, strlen(ssid), rssi);
}

TEST(strongest_first_one_entry_a_name) {
    count = 0;
    add("cafe", -70);
    add("home", -40);
    add("cafe", -80);                   // weaker reading: kept at -70
    add("work", -60);
    add("cafe", -50);                   // stronger: moves up
    CHECK_EQ(count, 3);
    CHECK(!strcmp(list[0].ssid, "home") && list[0].rssi == -40);
    CHECK(!strcmp(list[1].ssid, "cafe") && list[1].rssi == -50);
    CHECK(!strcmp(list[2].ssid, "work") && list[2].rssi == -60);
}

TEST(hidden_and_odd_names_left_out) {
    count = 0;
    wifi_names_add(list, &count, (const uint8_t *)"", 0, -30);              // hidden
    wifi_names_add(list, &count, (const uint8_t *)"a\0b", 3, -30);          // a NUL inside
    wifi_names_add(list, &count, (const uint8_t *)"123456789012345678901234567890123", 33, -30);
    CHECK_EQ(count, 0);
    wifi_names_add(list, &count, (const uint8_t *)"12345678901234567890123456789012xx", 32, -30);
    CHECK_EQ(count, 1);                 // 32 bytes: the most an SSID has, terminated here
    CHECK_EQ(strlen(list[0].ssid), 32);
}

TEST(a_prefix_is_another_name) {
    count = 0;
    add("net", -50);
    add("net2", -60);
    wifi_names_add(list, &count, (const uint8_t *)"net2xyz", 3, -40);       // "net", stronger
    CHECK_EQ(count, 2);
    CHECK(!strcmp(list[0].ssid, "net") && list[0].rssi == -40);
}

TEST(full_list_keeps_the_strongest) {
    count = 0;
    char name[8];
    for (int i = 0; i < WIFI_NAMES_MAX; i++) {
        snprintf(name, sizeof(name), "n%d", i);
        add(name, -50 - i);             // n0 -50 ... n15 -65
    }
    CHECK_EQ(count, WIFI_NAMES_MAX);
    add("weak", -90);                   // weaker than all: not taken
    CHECK_EQ(count, WIFI_NAMES_MAX);
    for (unsigned i = 0; i < count; i++)
        CHECK(strcmp(list[i].ssid, "weak"));
    add("strong", -20);                 // pushes out the weakest (n15)
    CHECK_EQ(count, WIFI_NAMES_MAX);
    CHECK(!strcmp(list[0].ssid, "strong"));
    CHECK(!strcmp(list[WIFI_NAMES_MAX - 1].ssid, "n14"));
    for (unsigned i = 1; i < count; i++)
        CHECK(list[i - 1].rssi >= list[i].rssi);
}

TEST_MAIN(
    RUN(strongest_first_one_entry_a_name);
    RUN(hidden_and_odd_names_left_out);
    RUN(a_prefix_is_another_name);
    RUN(full_list_keeps_the_strongest);
)
