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
static struct { char path[64]; char data[65536]; size_t len; } files[FILES];

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


static bool fail_renames;
static bool have_rotated;               // the card has a /var/log/.rotated
static int64_t card_rotated;
static int renames;

static bool f_rotate_file(const char *path, const char **why) {
    renames++;
    if (fail_renames) {
        *why = "denied";
        return false;
    }
    char old[64];
    snprintf(old, sizeof(old), "%s.1", path);
    bool exists = false;
    for (int i = 0; i < FILES; i++)
        exists |= !strcmp(files[i].path, path);
    if (!exists)
        return true;                        // nothing new: keep the old .1
    for (int i = 0; i < FILES; i++)         // remove the old .1
        if (!strcmp(files[i].path, old))
            memset(&files[i], 0, sizeof(files[i]));
    for (int i = 0; i < FILES; i++)
        if (!strcmp(files[i].path, path))
            snprintf(files[i].path, sizeof(files[i].path), "%s", old);
    return true;
}
static bool f_load_rotated(int64_t *s) {
    if (have_rotated)
        *s = card_rotated;
    return have_rotated;
}
static bool f_save_rotated(int64_t s, const char **why) {
    (void)why;
    card_rotated = s;
    have_rotated = true;
    return true;
}

static uint64_t mount_tried;            // when the last mount attempt by anyone ended
static uint64_t f_last_mount(void) { return mount_tried; }

static const logfile_hooks_t file_hooks = {
    f_now, f_wall, f_ready, f_generation, f_append, f_boot, f_retry, f_rotate_file,
    f_load_rotated, f_save_rotated, f_last_mount,
};

static void reset(void) {
    now = 1000000;                      // 1 s after boot
    clock_set = true;
    wall_at_boot = 1764165792000000;    // 2025-11-26T14:03:12Z
    card = true;
    generation = 1;
    fail_writes = false;
    appends = 0;
    fail_renames = false;
    have_rotated = false;
    card_rotated = 0;
    renames = 0;
    retries = 0;
    mount_tried = 0;
    card_inserted = false;
    memset(files, 0, sizeof(files));
    log_init(&log_hooks);
    log_set_deferred(true);
    log_set_history_level(SOS_LOG_INFO);
    logfile_init(&file_hooks);
    logfile_set_level(SOS_LOG_INFO);
    logfile_set_rotation(7, 0);
}

#define DAY_US (86400 * 1000000LL)
#define WALL_S(us) ((wall_at_boot + (int64_t)(us)) / 1000000)

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

// The first mount with the clock set starts the count: nothing is rotated, the time is kept.
TEST(rotation_count_starts_at_the_first_mount) {
    reset();
    log_msg(SOS_LOG_AUTH | SOS_LOG_WARNING, "x");
    logfile_poll();
    CHECK(have_rotated);
    CHECK_EQ(card_rotated, WALL_S(now));
    CHECK_EQ(renames, 0);
    logfile_status_t st;
    logfile_status(&st);
    CHECK(st.rotated_known);
    // 2025-11-26 14:03 + 7 days, at 00:00: 2025-12-03T00:00:00Z.
    CHECK_EQ(st.next_rotation, 1764720000);
}

// Due: the files become .1 (replacing the old .1), new ones start with a marker, and the time
// is kept; it happens again a period later, not before.
TEST(rotation_when_due) {
    reset();
    log_msg(SOS_LOG_AUTH | SOS_LOG_WARNING, "week one");
    logfile_poll();
    now = (uint64_t)((1764720000LL - 1) * 1000000 - wall_at_boot);   // a second before
    log_msg(SOS_LOG_AUTH | SOS_LOG_WARNING, "just before");
    logfile_poll();
    CHECK_EQ(renames, 0);
    now += 1000000;
    log_msg(SOS_LOG_AUTH | SOS_LOG_WARNING, "week two");
    logfile_poll();
    CHECK_EQ(renames, SOS_LOG_FACILITIES);
    const char *old = file("/var/log/auth.log.1");
    CHECK(strstr(old, "week one") && strstr(old, "just before"));
    CHECK(!strstr(old, "week two"));
    const char *cur = file("/var/log/auth.log");
    CHECK(strstr(cur, "--- rotated; this boot: SOS test"));
    CHECK(strstr(cur, "week two"));
    CHECK_EQ(card_rotated, 1764720000);
    // The notice about it is in the new daemon.log.
    settle();
    CHECK(strstr(file("/var/log/daemon.log"), "log: files rotated"));
    // A day later: not again. A week later: again, and week one's file is gone.
    int r = renames;
    now += (uint64_t)DAY_US;
    logfile_poll();
    CHECK_EQ(renames, r);
    now += (uint64_t)(6 * DAY_US);
    log_msg(SOS_LOG_AUTH | SOS_LOG_WARNING, "week three");
    logfile_poll();
    CHECK_EQ(renames, r + SOS_LOG_FACILITIES);
    CHECK(!strstr(file("/var/log/auth.log.1"), "week one"));
    CHECK(strstr(file("/var/log/auth.log.1"), "week two"));
}

// The kept time survives a reboot (a new writer reads it), and a rotation missed while the board
// was off happens at the first chance.
TEST(missed_rotation_happens_after_boot) {
    reset();
    have_rotated = true;
    card_rotated = 1764165792 - 10 * 86400;     // ten days before "now": overdue
    log_msg(SOS_LOG_KERN | SOS_LOG_WARNING, "after boot");
    logfile_poll();
    CHECK_EQ(renames, SOS_LOG_FACILITIES);
    CHECK_EQ(card_rotated, WALL_S(now));
}

TEST(no_clock_no_rotation) {
    reset();
    clock_set = false;
    have_rotated = true;
    card_rotated = 1;                           // long overdue
    log_msg(SOS_LOG_KERN | SOS_LOG_WARNING, "x");
    settle();
    CHECK_EQ(renames, 0);
}

TEST(rotation_time_of_day_and_period) {
    reset();
    logfile_set_rotation(1, 90);                // daily at 01:30
    have_rotated = true;
    card_rotated = 1764165792;                  // 2025-11-26 14:03
    logfile_poll();
    logfile_status_t st;
    logfile_status(&st);
    CHECK_EQ(st.next_rotation, 1764207000);     // 2025-11-27T01:30:00Z
    CHECK_EQ(st.rotate_days, 1);
    CHECK_EQ(st.rotate_minute, 90);
}

// By hand: rotates now, and the count starts again from today.
TEST(rotate_by_hand) {
    reset();
    log_msg(SOS_LOG_USER | SOS_LOG_NOTICE, "before");   // buffered, not yet written
    logfile_poll();
    CHECK(logfile_rotate() == NULL);
    CHECK(strstr(file("/var/log/user.log.1"), "before"));   // what was buffered went to the old file
    CHECK_EQ(card_rotated, WALL_S(now));
    card = false;
    CHECK(!strcmp(logfile_rotate(), "no card mounted"));
}

TEST(a_failed_rename_stops_the_files) {
    reset();
    logfile_poll();
    fail_renames = true;
    CHECK(!strcmp(logfile_rotate(), "denied"));
    logfile_status_t st;
    logfile_status(&st);
    CHECK_EQ(st.state, LOGFILE_STOPPED);
    CHECK(!strcmp(st.error_path, "/var/log/syslog"));
}

// A time kept while the clock was wrong, more than a day ahead, doesn't hold rotation back.
TEST(rotation_time_in_the_future_is_reset) {
    reset();
    have_rotated = true;
    card_rotated = 1764165792 + 100 * 86400;
    logfile_poll();
    CHECK_EQ(card_rotated, WALL_S(now));
    CHECK_EQ(renames, 0);
}

// Another card has its own time: read again after each mount.
TEST(rotation_time_read_again_after_a_mount) {
    reset();
    have_rotated = true;
    card_rotated = 1764165792;
    logfile_poll();
    card_rotated = 1764165792 - 30 * 86400;     // a different card, overdue
    generation++;
    logfile_poll();
    CHECK_EQ(renames, SOS_LOG_FACILITIES);
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

// A failed mount by someone else (`sd speed 75`: about 4 s on a card that won't initialise) must
// not be followed by the quiet retry in the same main-loop pass: the two made 8 s, the watchdog's
// limit, on dev-1. The retry waits LOGFILE_RETRY_MS after any mount attempt.
TEST(no_retry_right_after_another_mount) {
    reset();
    now = 10 * LOGFILE_RETRY_MS * 1000ull;  // long after boot: a retry would be due
    card = false;                           // the card just went (sd speed's failed mount)
    mount_tried = now;                      // ...which has just ended
    logfile_poll();
    CHECK_EQ(retries, 0);
    now += LOGFILE_RETRY_MS * 1000ull - 1;
    logfile_poll();
    CHECK_EQ(retries, 0);
    now += 1;
    logfile_poll();
    CHECK_EQ(retries, 1);
    // Its own retries keep their minute too.
    now += 1000000;
    logfile_poll();
    CHECK_EQ(retries, 1);
}

TEST(boot_marker_has_the_boot_time) {
    reset();
    now = 30000000;                     // written 30 s after boot
    log_msg(SOS_LOG_KERN | SOS_LOG_ERR, "x");
    logfile_poll();
    CHECK(strstr(file("/var/log/kern.log"), "2025-11-26T14:03:12.000Z --- boot:"));
}

// Each mount's files start with a marker; "rotated" only after a rotation.
TEST(markers_after_a_card_change) {
    reset();
    log_msg(SOS_LOG_AUTH | SOS_LOG_WARNING, "first card");
    logfile_poll();
    CHECK(logfile_rotate() == NULL);
    log_msg(SOS_LOG_AUTH | SOS_LOG_WARNING, "after rotating");
    logfile_poll();
    CHECK_EQ(count(file("/var/log/auth.log"), "--- rotated; this boot:"), 1);
    // Another card: auth.log gets a "card mounted" marker; kern.log, never written, a boot one.
    memset(files, 0, sizeof(files));
    generation++;
    log_msg(SOS_LOG_AUTH | SOS_LOG_WARNING, "second card");
    log_msg(SOS_LOG_KERN | SOS_LOG_WARNING, "kern on the second card");
    settle();
    const char *a = file("/var/log/auth.log");
    CHECK_EQ(count(a, "--- card mounted; this boot: SOS test"), 1);
    CHECK(!strstr(a, "rotated"));
    CHECK(strstr(a, "--- card mounted") < strstr(a, "second card"));
    CHECK_EQ(count(file("/var/log/kern.log"), "--- boot: SOS test"), 1);
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

// A file nothing was logged to since the last rotation keeps its .1 (the period before).
TEST(a_quiet_file_keeps_its_old_copy) {
    reset();
    log_msg(SOS_LOG_KERN | SOS_LOG_WARNING, "kern before");
    log_msg(SOS_LOG_AUTH | SOS_LOG_WARNING, "auth before");
    logfile_poll();
    CHECK(logfile_rotate() == NULL);
    log_msg(SOS_LOG_AUTH | SOS_LOG_WARNING, "auth between");
    logfile_poll();
    CHECK(logfile_rotate() == NULL);
    CHECK(strstr(file("/var/log/kern.log.1"), "kern before"));   // kern: nothing new, kept
    CHECK(strstr(file("/var/log/auth.log.1"), "auth between"));  // auth: replaced
    CHECK(!strstr(file("/var/log/auth.log.1"), "auth before"));
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
    RUN(rotation_count_starts_at_the_first_mount);
    RUN(rotation_when_due);
    RUN(missed_rotation_happens_after_boot);
    RUN(no_clock_no_rotation);
    RUN(rotation_time_of_day_and_period);
    RUN(rotate_by_hand);
    RUN(a_failed_rename_stops_the_files);
    RUN(rotation_time_in_the_future_is_reset);
    RUN(rotation_time_read_again_after_a_mount);
    RUN(no_card_retries_every_minute);
    RUN(no_retry_right_after_another_mount);
    RUN(boot_marker_has_the_boot_time);
    RUN(markers_after_a_card_change);
    RUN(lowering_the_file_level_keeps_its_own_record);
    RUN(flush_writes_everything_now);
    RUN(a_quiet_file_keeps_its_old_copy);
    RUN(not_started);
    RUN(paths);
)
