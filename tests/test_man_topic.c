// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/man/man_topic.c: what a manual page's topic may be, and the topic a file in
// /man holds (for Tab after `man`).

#include "man/man_topic.h"
#include "test.h"

static bool ok(const char *s) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", s);
    return man_topic_ok(buf);
}

TEST(topics_are_command_names) {
    CHECK(ok("wifi") && ok("ssh-keys") && ok("log_rotate") && ok("x2"));
    CHECK(!ok("") && !ok("-x") && !ok("_x") && !ok("a b") && !ok("../etc") && !ok("a.txt"));
    CHECK(ok("12345678901234567890123456789012"));             // 32
    CHECK(!ok("123456789012345678901234567890123"));           // 33
    char up[] = "WiFi";
    CHECK(man_topic_ok(up) && !strcmp(up, "wifi"));            // lower-cased
}

TEST(topic_of_a_page_file) {
    char t[MAN_TOPIC_MAX + 1];
    CHECK(man_topic_of("wifi.txt", t, sizeof(t)) && !strcmp(t, "wifi"));
    CHECK(man_topic_of("LOG.TXT", t, sizeof(t)) && !strcmp(t, "log"));   // FAT: any case
    CHECK(!man_topic_of("wifi.~new", t, sizeof(t)));           // still downloading
    CHECK(!man_topic_of("wifi", t, sizeof(t)));
    CHECK(!man_topic_of(".txt", t, sizeof(t)));
    CHECK(!man_topic_of("notes.md", t, sizeof(t)));
    CHECK(!man_topic_of("a b.txt", t, sizeof(t)));
    CHECK(!man_topic_of("\x1b[2J.txt", t, sizeof(t)));       // nothing that isn't a topic
    CHECK(!man_topic_of("123456789012345678901234567890123.txt", t, sizeof(t)));
    char small[4];
    CHECK(!man_topic_of("wifi.txt", small, sizeof(small)));   // doesn't fit: not taken
    CHECK(man_topic_of("ls.txt", small, sizeof(small)) && !strcmp(small, "ls"));
}

TEST_MAIN(
    RUN(topics_are_command_names);
    RUN(topic_of_a_page_file);
)
