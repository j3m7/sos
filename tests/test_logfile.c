// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/logfile.c with src/core/log.c: messages reach the file of their
// facility with UTC or since-boot times, boot markers, buffering and when buffers are written,
// no card, a failed write and the next mount, messages lost before they were written, and the
// file level.

#include <stdint.h>
#include <stdlib.h>

#include "core/log.h"
#include "core/logfile.h"
#include "test.h"

// --- fake platform -------------------------------------------------------------------------------

static uint64_t now;                    // time since boot
static bool clock_set;
static int64_t wall_at_boot;            // UTC at boot, µs
static bool card;
static uint32_t generation;
static bool fail_writes;
static int appends;

#define FILES 8
static struct { char path[48]; char data[65536]; size_t len; } files[FILES];

static uint64_t f_now(void) { return now; }
static bool f_in_irq(void) { return false; }
static uint32_t f_lock(void) { return 0; }
static void f_unlock(uint32_t s) { (void)s; }
static void f_write(const char *s, size_t n) { (void)s; (void)n; }
static void f_nop(void) {}
static const log_hooks_t log_hooks = {
    f_now, f_in_irq, f_lock, f_unlock, f_write, f_nop, f_nop, NULL,
};

static bool f_wall(int64_t *us) {
    if (clock_set)
        *us = wall_at_boot + (int64_t)now;
    return clock_set;
}
static bool f_ready(void) { return card; }
static uint32_t f_generation(void) { return generation; }

static const char *file(const char *path) {
    for (int i = 0; i < FILES; i++)
        if (!strcmp(files[i].path, path))
            return files[i].data;
    return "";
}

static bool f_append(const char *path, const char *data, size_t n, const char **why) {
    appends++;
    if (fail_writes) {
        *why = "disk error";
        return false;
    }
    int i = 0;
    while (i < FILES && files[i].path[0] && strcmp(files[i].path, path))
        i++;
    CHECK(i < FILES);
    snprintf(files[i].path, sizeof(files[i].path), "%s", path);
    CHECK(files[i].len + n < sizeof(files[i].data));
    memcpy(files[i].data + files[i].len, data, n);
    files[i].len += n;
    files[i].data[files[i].len] = '\0';
    return true;
}

static void f_boot(char *buf, size_t len) { snprintf(buf, len, "SOS test, last reset: power-on"); }

static int retries;
static bool card_inserted;              // a retry finds a card
static void f_retry(void) {
    retries++;
    if (card_inserted && !card) {
        card = true;
        generation++;
    }
}

static const logfile_hooks_t file_hooks = {
    f_now, f_wall, f_ready, f_generation, f_append, f_boot, f_retry,
};

static void reset(void) {
    now = 1000000;                      // 1 s after boot
    clock_set = true;
    wall_at_boot = 1764165792000000;    // 2025-11-26T14:03:12Z
    card = true;
    generation = 1;
    fail_writes = false;
    appends = 0;
    retries = 0;
    card_inserted = false;
    memset(files, 0, sizeof(files));
    log_init(&log_hooks);
    log_set_deferred(true);
    log_set_history_level(SOS_LOG_INFO);
    logfile_init(&file_hooks);
    logfile_set_level(SOS_LOG_INFO);
}

// Poll until nothing more is written, letting time pass so every buffer becomes due.
static void settle(void) {
    for (int i = 0; i < 20; i++) {
        now += LOGFILE_FLUSH_MS * 1000ull;
        logfile_poll();
    }
}

static int count(const char *hay, const char *needle) {
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)); p++)
        n++;
    return n;
}

// --- tests ---------------------------------------------------------------------------------------

TEST(each_facility_has_its_file) {
    reset();
    log_msg(SOS_LOG_KERN | SOS_LOG_NOTICE, "kern line");
    log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "auth line");
    log_msg(SOS_LOG_DAEMON | SOS_LOG_INFO, "daemon line");
    log_msg(SOS_LOG_USER | SOS_LOG_NOTICE, "user line");
    log_msg(SOS_LOG_INFO, "untagged line");
    settle();
    CHECK(strstr(file("/var/log/kern.log"), "notice kern line\n"));
    CHECK(strstr(file("/var/log/auth.log"), "notice auth line\n"));
    CHECK(strstr(file("/var/log/daemon.log"), "info daemon line\n"));
    CHECK(strstr(file("/var/log/user.log"), "notice user line\n"));
    CHECK(strstr(file("/var/log/syslog"), "info untagged line\n"));
    // One file each, never two.
    CHECK(!strstr(file("/var/log/auth.log"), "kern line"));
    CHECK(!strstr(file("/var/log/syslog"), "auth line"));
    CHECK(!strstr(file("/var/log/kern.log"), "untagged"));
    CHECK_EQ(count(file("/var/log/auth.log"), "auth line"), 1);
}

TEST(lines_have_utc_time_level_and_text) {
    reset();
    now = 2345000;                      // logged 2.345 s after boot
    log_msg(SOS_LOG_AUTH | SOS_LOG_WARNING, "console: wrong password");
    now = 10000000;                     // written later: the time is still when it was logged
    logfile_poll();
    CHECK(strstr(file("/var/log/auth.log"),
                  "2025-11-26T14:03:14.345Z warning console: wrong password\n"));
    // The level's tag in the history ("warning: ") isn't repeated.
    CHECK(!strstr(file("/var/log/auth.log"), "warning: "));
}

TEST(without_a_clock_times_are_since_boot) {
    reset();
    clock_set = false;
    now = 12345000;
    log_msg(SOS_LOG_KERN | SOS_LOG_ERR, "no clock");
    logfile_poll();
    CHECK(strstr(file("/var/log/kern.log"), "+12.345 err no clock\n"));
}

TEST(boot_marker_starts_each_file_once) {
    reset();
    log_msg(SOS_LOG_AUTH | SOS_LOG_WARNING, "one");
    logfile_poll();
    log_msg(SOS_LOG_AUTH | SOS_LOG_WARNING, "two");
    logfile_poll();
    const char *a = file("/var/log/auth.log");
    CHECK_EQ(count(a, "--- boot: SOS test, last reset: power-on ---"), 1);
    CHECK(strstr(a, "--- boot") < strstr(a, "one"));
    CHECK(file("/var/log/kern.log")[0] == '\0');    // no messages, no marker either
}

TEST(warnings_are_written_at_once_others_wait) {
    reset();
    log_msg(SOS_LOG_DAEMON | SOS_LOG_NOTICE, "routine");
    logfile_poll();
    CHECK_EQ(appends, 0);               // waits for more, or for LOGFILE_FLUSH_MS
    now += LOGFILE_FLUSH_MS * 1000ull - 1;
    logfile_poll();
    CHECK_EQ(appends, 0);
    now += 1;
    logfile_poll();
    CHECK_EQ(appends, 1);
    CHECK(strstr(file("/var/log/daemon.log"), "routine"));

    log_msg(SOS_LOG_DAEMON | SOS_LOG_WARNING, "trouble");
    logfile_poll();
    CHECK_EQ(appends, 2);
    CHECK(strstr(file("/var/log/daemon.log"), "trouble"));
}

TEST(a_full_buffer_is_written_and_nothing_is_lost) {
    reset();
    for (int i = 0; i < 40; i++)        // more than one buffer's worth, and the history holds it
        log_msg(SOS_LOG_USER | SOS_LOG_INFO, "line %02d with some text to fill the buffer up", i);
    logfile_poll();
    CHECK(appends >= 1);                // full: written without waiting
    settle();
    const char *u = file("/var/log/user.log");
    for (int i = 0; i < 40; i++) {
        char want[32];
        snprintf(want, sizeof(want), "line %02d ", i);
        CHECK_EQ(count(u, want), 1);
    }
    logfile_status_t st;
    logfile_status(&st);
    CHECK_EQ(st.lost, 0);
    CHECK_EQ(st.lines, 40);
    CHECK_EQ(st.bytes, strlen(u));
    CHECK_EQ(st.buffered, 0);
}

TEST(no_card_messages_wait_in_the_history) {
    reset();
    card = false;
    log_msg(SOS_LOG_AUTH | SOS_LOG_WARNING, "while no card");
    logfile_poll();
    logfile_status_t st;
    logfile_status(&st);
    CHECK_EQ(st.state, LOGFILE_NO_CARD);
    CHECK_EQ(appends, 0);
    card = true;
    generation++;
    logfile_poll();
    CHECK(strstr(file("/var/log/auth.log"), "while no card"));
    logfile_status(&st);
    CHECK_EQ(st.state, LOGFILE_WRITING);
}

TEST(messages_dropped_before_writing_are_counted_in_every_file) {
    reset();
    card = false;
    for (int i = 0; i < 300; i++)       // overflows the 4 KB history
        log_msg(SOS_LOG_DAEMON | SOS_LOG_INFO, "flood %03d", i);
    card = true;
    settle();
    logfile_status_t st;
    logfile_status(&st);
    CHECK(st.lost > 0);
    const char *d = file("/var/log/daemon.log");
    CHECK(strstr(d, "messages lost before they were written]"));
    CHECK(strstr(file("/var/log/auth.log"), "messages lost before they were written]"));
    CHECK(strstr(d, "flood 299"));
    CHECK(!strstr(d, "flood 000"));
    // Every message is either written or counted.
    CHECK_EQ(count(d, "flood ") + (int)st.lost, 300);
}

TEST(a_failed_write_stops_until_the_next_mount) {
    reset();
    fail_writes = true;
    log_msg(SOS_LOG_AUTH | SOS_LOG_ERR, "first");
    logfile_poll();
    logfile_status_t st;
    logfile_status(&st);
    CHECK_EQ(st.state, LOGFILE_STOPPED);
    CHECK(st.error && !strcmp(st.error, "disk error"));
    CHECK(st.error_path && !strcmp(st.error_path, "/var/log/auth.log"));
    // The failure is in the history (for dmesg), and nothing is tried again meanwhile.
    char buf[LOG_LINE_MAX + 1];
    uint32_t cur = 0;
    bool said = false;
    while (log_history_next(&cur, buf, sizeof(buf), NULL))
        said |= strstr(buf, "log files stopped until the card is mounted again") != NULL;
    CHECK(said);
    int tries = appends;
    log_msg(SOS_LOG_AUTH | SOS_LOG_ERR, "second");
    settle();
    CHECK_EQ(appends, tries);
    // Mounted again: everything that waited is written, the failure message too.
    fail_writes = false;
    generation++;
    settle();
    const char *a = file("/var/log/auth.log");
    CHECK(strstr(a, "err first"));
    CHECK(strstr(a, "err second"));
    CHECK(strstr(file("/var/log/daemon.log"), "log files stopped"));
    logfile_status(&st);
    CHECK_EQ(st.state, LOGFILE_WRITING);
    CHECK(st.error == NULL);
}

TEST(file_level_filters) {
    reset();
    logfile_set_level(SOS_LOG_NOTICE);
    log_msg(SOS_LOG_DAEMON | SOS_LOG_INFO, "info, not written");
    log_msg(SOS_LOG_DAEMON | SOS_LOG_NOTICE, "notice, written");
    settle();
    const char *d = file("/var/log/daemon.log");
    CHECK(!strstr(d, "info, not written"));
    CHECK(strstr(d, "notice, written"));
}

// A level change is logged just before it's made: raising the level mustn't drop that record,
// even though the writer hasn't taken it yet (or there's no card at that moment).
TEST(raising_the_file_level_keeps_its_own_record) {
    reset();
    card = false;
    log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: log file level err (until reboot)");
    logfile_set_level(SOS_LOG_ERR);
    log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "after the change, not written");
    card = true;
    settle();
    const char *a = file("/var/log/auth.log");
    CHECK(strstr(a, "config: log file level err"));
    CHECK(!strstr(a, "after the change"));
}

// With no card, a mount is tried every LOGFILE_RETRY_MS, not more often; one that finds a card
// starts the files.
TEST(no_card_retries_every_minute) {
    reset();
    card = false;
    logfile_init(&file_hooks);          // as logfile_start(): it has just tried
    log_msg(SOS_LOG_AUTH | SOS_LOG_WARNING, "waiting for a card");
    for (int i = 0; i < 100; i++) {
        now += 500000;                  // 50 s in all
        logfile_poll();
    }
    CHECK_EQ(retries, 0);
    now += 10000000;
    logfile_poll();
    CHECK_EQ(retries, 1);
    logfile_poll();
    CHECK_EQ(retries, 1);
    card_inserted = true;
    now += LOGFILE_RETRY_MS * 1000ull;
    logfile_poll();
    CHECK_EQ(retries, 2);
    CHECK(strstr(file("/var/log/auth.log"), "waiting for a card"));
    // Mounted: no more tries.
    now += 10 * LOGFILE_RETRY_MS * 1000ull;
    logfile_poll();
    CHECK_EQ(retries, 2);
}

TEST(boot_marker_has_the_boot_time) {
    reset();
    now = 30000000;                     // written 30 s after boot
    log_msg(SOS_LOG_KERN | SOS_LOG_ERR, "x");
    logfile_poll();
    CHECK(strstr(file("/var/log/kern.log"), "2025-11-26T14:03:12.000Z --- boot:"));
}

// Lowering the level again keeps its record too, though it's logged while the level is high.
TEST(lowering_the_file_level_keeps_its_own_record) {
    reset();
    logfile_set_level(SOS_LOG_ERR);
    log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: log file level info (until reboot)");
    logfile_set_level(SOS_LOG_INFO);
    settle();
    CHECK(strstr(file("/var/log/auth.log"), "config: log file level info"));
}

TEST(flush_writes_everything_now) {
    reset();
    log_msg(SOS_LOG_KERN | SOS_LOG_NOTICE, "reboot (from console)");
    logfile_poll();
    CHECK_EQ(appends, 0);               // would wait for LOGFILE_FLUSH_MS
    logfile_flush();
    CHECK(strstr(file("/var/log/kern.log"), "reboot (from console)"));
    card = false;
    logfile_flush();                    // no card: nothing, no crash
}

TEST(not_started) {
    logfile_init(NULL);
    logfile_status_t st;
    logfile_status(&st);
    CHECK_EQ(st.state, LOGFILE_OFF);
    logfile_poll();                     // must not crash
}

TEST(paths) {
    CHECK(!strcmp(logfile_path(SOS_LOG_AUTH | SOS_LOG_ERR), "/var/log/auth.log"));
    CHECK(!strcmp(logfile_path(SOS_LOG_NONE), "/var/log/syslog"));
    CHECK(logfile_path(7 << 3) == NULL);
}

TEST_MAIN(
    RUN(each_facility_has_its_file);
    RUN(lines_have_utc_time_level_and_text);
    RUN(without_a_clock_times_are_since_boot);
    RUN(boot_marker_starts_each_file_once);
    RUN(warnings_are_written_at_once_others_wait);
    RUN(a_full_buffer_is_written_and_nothing_is_lost);
    RUN(no_card_messages_wait_in_the_history);
    RUN(messages_dropped_before_writing_are_counted_in_every_file);
    RUN(a_failed_write_stops_until_the_next_mount);
    RUN(file_level_filters);
    RUN(raising_the_file_level_keeps_its_own_record);
    RUN(no_card_retries_every_minute);
    RUN(boot_marker_has_the_boot_time);
    RUN(lowering_the_file_level_keeps_its_own_record);
    RUN(flush_writes_everything_now);
    RUN(not_started);
    RUN(paths);
)
