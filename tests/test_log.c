// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/log.c: levels, console output with input-line redraw, deferred printing
// from interrupt context, and the RAM history.

#include <stdint.h>

#include "core/log.h"
#include "test.h"

// --- fake platform -------------------------------------------------------------------------------

static uint64_t now = 1234000;      // 1.234 s
static bool irq;
static int locked, hides, shows;
static char out[16384];
static size_t out_len;
static bool log_from_write;         // make the console writer log a message (re-entrancy)

static uint64_t f_now(void) { return now; }
static bool f_in_irq(void) { return irq; }
static uint32_t f_lock(void) { CHECK(!locked); locked++; return 42; }
static void f_unlock(uint32_t s) { CHECK_EQ(s, 42); locked--; }
static void f_write(const char *s, size_t n) {
    CHECK(!locked);                             // never write while holding the lock
    CHECK(out_len + n < sizeof(out));
    memcpy(out + out_len, s, n);
    out_len += n;
    out[out_len] = '\0';
    if (log_from_write) {
        log_from_write = false;
        log_msg(SOS_LOG_NOTICE, "logged while writing");
    }
}
static void f_hide(void) { hides++; f_write("<hide>", 6); }
static void f_show(void) { shows++; f_write("<show>", 6); }

static bool held;
static bool f_hold(void) { return held; }

static const log_hooks_t hooks = {
    f_now, f_in_irq, f_lock, f_unlock, f_write, f_hide, f_show, f_hold,
};

static void reset(void) {
    now = 1234000;
    irq = false;
    held = false;
    locked = hides = shows = 0;
    out_len = 0;
    out[0] = '\0';
    log_from_write = false;
    log_init(&hooks);
    log_set_console_level(SOS_LOG_NOTICE);
    log_set_history_level(SOS_LOG_INFO);
}

static int history_count(void) {
    char buf[LOG_LINE_MAX + 1];
    uint32_t cur = 0;
    int n = 0;
    while (log_history_next(&cur, buf, sizeof(buf)))
        n++;
    return n;
}

#define CHECK_OUT(expected) do {                                                            \
        test_checks++;                                                                      \
        if (strcmp(out, (expected)) != 0) {                                                 \
            test_failures++;                                                                \
            fprintf(stderr, "%s:%d: %s: console\n  got:      \"%s\"\n  expected: \"%s\"\n",  \
                    __FILE__, __LINE__, test_current, out, (expected));                     \
        }                                                                                   \
    } while (0)

// --- tests ---------------------------------------------------------------------------------------

TEST(notice_printed_with_redraw) {
    reset();
    log_msg(SOS_LOG_NOTICE, "sshd: hello %d", 7);
    CHECK_OUT("<hide>[    1.234] sshd: hello 7\n<show>");
    CHECK_EQ(hides, 1);
    CHECK_EQ(shows, 1);
}

TEST(levels_and_tags) {
    reset();
    log_msg(SOS_LOG_ERR, "bad");
    out_len = 0;
    log_msg(SOS_LOG_WARNING, "careful");
    CHECK_OUT("<hide>[    1.234] warning: careful\n<show>");
    out_len = 0; out[0] = '\0';
    log_msg(SOS_LOG_INFO, "quiet");             // stored, not printed
    CHECK_OUT("");
    CHECK_EQ(hides, 2);                         // no redraw for nothing
    log_msg(SOS_LOG_DEBUG, "dropped");          // below the history level: not even stored
    CHECK_EQ(history_count(), 3);
    char buf[LOG_LINE_MAX + 1];
    uint32_t cur = 0;
    CHECK(log_history_next(&cur, buf, sizeof(buf)));
    CHECK(!strcmp(buf, "[    1.234] error: bad"));
}

TEST(console_level_is_adjustable) {
    reset();
    log_set_console_level(SOS_LOG_ERR);
    log_msg(SOS_LOG_WARNING, "hidden");
    CHECK_OUT("");
    log_set_console_level(SOS_LOG_DEBUG);
    log_set_history_level(SOS_LOG_DEBUG);
    log_msg(SOS_LOG_DEBUG, "shown");
    CHECK_OUT("<hide>[    1.234] shown\n<show>");
    CHECK_EQ(history_count(), 2);               // both in the history
}

TEST(interrupt_context_defers_printing) {
    reset();
    irq = true;
    log_msg(SOS_LOG_NOTICE, "from irq 1");
    log_msg(SOS_LOG_NOTICE, "from irq 2");
    log_flush();                                // flush is a no-op in interrupt context
    CHECK_OUT("");
    CHECK_EQ(history_count(), 2);
    irq = false;
    log_flush();
    CHECK_OUT("<hide>[    1.234] from irq 1\n[    1.234] from irq 2\n<show>");  // one redraw
    out_len = 0; out[0] = '\0';
    log_flush();                                // nothing new
    CHECK_OUT("");
}

TEST(deferred_mode_waits_for_flush) {
    reset();
    log_set_deferred(true);
    log_msg(SOS_LOG_NOTICE, "during a command");
    CHECK_OUT("");                              // not printed in the middle of command output
    log_flush();
    CHECK_OUT("<hide>[    1.234] during a command\n<show>");
    reset();                                    // log_init turns deferred mode off again
    log_msg(SOS_LOG_NOTICE, "boot");
    CHECK_OUT("<hide>[    1.234] boot\n<show>");
}

TEST(timestamp_format) {
    reset();
    now = 98765432100ull;                       // 98765.432 s
    log_msg(SOS_LOG_NOTICE, "x");
    CHECK_OUT("<hide>[98765.432] x\n<show>");
}

TEST(message_is_one_line) {
    reset();
    log_msg(SOS_LOG_NOTICE, "a\nb\n\n");        // inner newline flattened, trailing ones dropped
    CHECK_OUT("<hide>[    1.234] a b\n<show>");
}

TEST(control_characters_removed) {
    reset();
    // As from an SSH client's user name: escape sequence, carriage return, tab, DEL, 8-bit CSI.
    log_msg(SOS_LOG_NOTICE, "user %s", "x\x1b[2J\rroot\tA\x7f\x9b");
    CHECK_OUT("<hide>[    1.234] user x [2J root A??\n<show>");
}

TEST(long_message_truncated) {
    char big[400];
    memset(big, 'z', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    reset();
    log_msg(SOS_LOG_NOTICE, "%s", big);
    char buf[LOG_LINE_MAX + 8];
    uint32_t cur = 0;
    CHECK(log_history_next(&cur, buf, sizeof(buf)));
    CHECK_EQ(strlen(buf), LOG_LINE_MAX);
}

TEST(history_cursor_continues) {
    char buf[LOG_LINE_MAX + 1];
    uint32_t cur = 0;
    reset();
    log_msg(SOS_LOG_INFO, "one");
    log_msg(SOS_LOG_INFO, "two");
    CHECK(log_history_next(&cur, buf, sizeof(buf)) && strstr(buf, "one"));
    CHECK(log_history_next(&cur, buf, sizeof(buf)) && strstr(buf, "two"));
    CHECK(!log_history_next(&cur, buf, sizeof(buf)));
    log_msg(SOS_LOG_INFO, "three");
    CHECK(log_history_next(&cur, buf, sizeof(buf)) && strstr(buf, "three"));
}

TEST(history_drops_oldest_when_full) {
    char buf[LOG_LINE_MAX + 1];
    uint32_t cur = 0, stale;
    reset();
    log_msg(SOS_LOG_INFO, "first");
    CHECK(log_history_next(&cur, buf, sizeof(buf)));
    stale = 0;                                  // a reader that never started
    for (int i = 0; i < 400; i++)               // ~20 bytes each: far more than the history
        log_msg(SOS_LOG_INFO, "msg %03d", i);
    int n = history_count();
    CHECK(n > 100 && n < 400);
    CHECK(log_history_next(&stale, buf, sizeof(buf)));
    CHECK(strstr(buf, "msg ") && !strstr(buf, "first"));   // oldest kept is a later one
    CHECK(log_history_next(&cur, buf, sizeof(buf)));       // a stale cursor jumps forward
    CHECK(strstr(buf, "msg "));
    // The newest message is intact.
    const char *last = NULL;
    cur = 0;
    while (log_history_next(&cur, buf, sizeof(buf)))
        last = buf;
    CHECK(last && strstr(buf, "msg 399"));
}

TEST(overflow_before_printing_is_reported) {
    reset();
    irq = true;                                 // nothing printed while these pile up
    for (int i = 0; i < 400; i++)
        log_msg(SOS_LOG_NOTICE, "burst %03d", i);
    irq = false;
    log_flush();
    CHECK(strstr(out, "[log: some messages were dropped before they were shown]") != NULL);
    CHECK(strstr(out, "burst 000") == NULL);
    CHECK(strstr(out, "burst 399") != NULL);
    CHECK_EQ(hides, 1);
    CHECK_EQ(shows, 1);
}

TEST(held_console_keeps_messages_for_later) {
    reset();
    held = true;                                // a full-screen program on the console
    log_msg(SOS_LOG_NOTICE, "while held");
    log_flush();
    CHECK_EQ(out_len, 0);
    CHECK_EQ(hides, 0);
    CHECK_EQ(history_count(), 1);               // in the history meanwhile
    log_msg(SOS_LOG_INFO, "not for the console");
    log_msg(SOS_LOG_WARNING, "also held");
    CHECK_EQ(log_pending(), 2);                 // the info message won't be printed
    held = false;
    log_flush();
    CHECK(strstr(out, "<hide>") == out);
    CHECK(strstr(out, "also held\n<show>") != NULL);
    CHECK(strstr(out, "not for the console") == NULL);
    CHECK_EQ(log_pending(), 0);
}

TEST(held_console_overflow_is_reported) {
    reset();
    held = true;
    for (int i = 0; i < 400; i++)
        log_msg(SOS_LOG_NOTICE, "held %03d", i);
    CHECK_EQ(out_len, 0);
    int pending = log_pending();
    CHECK(pending > 1 && pending < 400);        // what's left, plus one for the dropped ones
    held = false;
    log_flush();
    CHECK(strstr(out, "[log: some messages were dropped before they were shown]") != NULL);
    CHECK(strstr(out, "held 399") != NULL);
}

TEST(logging_while_printing_does_not_recurse) {
    reset();
    log_from_write = true;
    log_msg(SOS_LOG_NOTICE, "outer");
    CHECK(strstr(out, "outer") != NULL);
    CHECK(strstr(out, "logged while writing") != NULL);    // printed by the same flush
    CHECK_EQ(hides, 1);
    CHECK_EQ(locked, 0);
}

TEST(level_names) {
    CHECK(!strcmp(log_level_name(SOS_LOG_WARNING), "warning"));
    CHECK(log_level_name(0) == NULL);
    CHECK(log_level_name(9) == NULL);
    CHECK_EQ(log_level_parse("debug"), SOS_LOG_DEBUG);
    CHECK_EQ(log_level_parse("err"), SOS_LOG_ERR);
    CHECK_EQ(log_level_parse("loud"), -1);
}

static void vlog_prefixed(int level, const char *prefix, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vmsg_prefixed(level, prefix, fmt, ap);
    va_end(ap);
}

// A prefix goes after the level's tag, where a caller's own "prefix: %s" used to put it.
TEST(prefix_goes_before_the_message) {
    reset();
    vlog_prefixed(SOS_LOG_NOTICE, "sshd: ", "accepted %s:%u", "10.0.0.2", 22u);
    vlog_prefixed(SOS_LOG_ERR, "sshd: ", "reset");
    CHECK_OUT("<hide>[    1.234] sshd: accepted 10.0.0.2:22\n<show>"
              "<hide>[    1.234] error: sshd: reset\n<show>");
}

// A message above the history level is dropped before anything is formatted: its arguments
// are never read (a %s with a bad pointer would crash here if they were).
TEST(prefixed_message_above_the_level_is_not_formatted) {
    reset();
    vlog_prefixed(SOS_LOG_DEBUG, "sshd: ", "%s", (const char *)(uintptr_t)1);
    CHECK_EQ(history_count(), 0);
    CHECK_OUT("");
}

// Prefix and message share the one line: together they are cut at LOG_LINE_MAX.
TEST(prefixed_long_message_truncated) {
    reset();
    char big[2 * LOG_LINE_MAX];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    vlog_prefixed(SOS_LOG_NOTICE, "sshd: ", "%s", big);
    char buf[2 * LOG_LINE_MAX];
    uint32_t cur = 0;
    CHECK(log_history_next(&cur, buf, sizeof(buf)));
    CHECK_EQ(strlen(buf), LOG_LINE_MAX);
    CHECK(!strncmp(buf, "[    1.234] sshd: xxx", 21));
}

TEST(nothing_before_init) {
    log_init(NULL);
    log_msg(SOS_LOG_ERR, "ignored");            // must not crash
    log_flush();
    uint32_t cur = 0;
    char buf[8];
    CHECK(!log_history_next(&cur, buf, sizeof(buf)));
}

TEST_MAIN(
    RUN(notice_printed_with_redraw);
    RUN(levels_and_tags);
    RUN(console_level_is_adjustable);
    RUN(interrupt_context_defers_printing);
    RUN(deferred_mode_waits_for_flush);
    RUN(timestamp_format);
    RUN(message_is_one_line);
    RUN(control_characters_removed);
    RUN(long_message_truncated);
    RUN(history_cursor_continues);
    RUN(history_drops_oldest_when_full);
    RUN(overflow_before_printing_is_reported);
    RUN(held_console_keeps_messages_for_later);
    RUN(held_console_overflow_is_reported);
    RUN(logging_while_printing_does_not_recurse);
    RUN(prefix_goes_before_the_message);
    RUN(prefixed_message_above_the_level_is_not_formatted);
    RUN(prefixed_long_message_truncated);
    RUN(level_names);
    RUN(nothing_before_init);
)
