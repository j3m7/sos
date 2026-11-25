// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/chat/chat.c: wall, chat and who between shells (with the real shell_core.c
// and termtext.c).

#include <stdint.h>
#include <stdlib.h>

#include "chat/chat.h"
#include "shell/shell.h"
#include "test.h"

// --- fake channels ---------------------------------------------------------------------------

typedef struct {
    char in[1024];
    size_t in_len, in_pos;
    char out[16384];
    size_t out_len;
    int space;                  // what write_space() reports
} fake_t;

static int fake_read(void *ctx, char *buf, int len) {
    fake_t *f = ctx;
    size_t n = f->in_len - f->in_pos;
    if (n > (size_t)len)
        n = (size_t)len;
    memcpy(buf, f->in + f->in_pos, n);
    f->in_pos += n;
    return (int)n;
}

static void fake_write(void *ctx, const char *buf, int len) {
    fake_t *f = ctx;
    CHECK(f->out_len + (size_t)len < sizeof(f->out));
    if (f->out_len + (size_t)len >= sizeof(f->out))
        return;
    memcpy(f->out + f->out_len, buf, (size_t)len);
    f->out_len += (size_t)len;
    f->out[f->out_len] = '\0';
}

static int fake_space(void *ctx) {
    return ((fake_t *)ctx)->space;
}

static const shell_io_t io = {
    .name = "ssh", .read = fake_read, .write = fake_write, .crlf = false, .write_space = fake_space,
};

void shell_execute(shell_t *sh, char *cmd) {
    char *args = strchr(cmd, ' ');
    if (args) {
        *args++ = '\0';
        while (*args == ' ')
            args++;
        if (!*args)
            args = NULL;
    }
    if (!strcmp(cmd, "wall"))
        chat_cmd_wall(sh, args);
    else if (!strcmp(cmd, "chat"))
        chat_cmd_chat(sh, args);
    else if (!strcmp(cmd, "who"))
        chat_cmd_who(sh, args);
    else
        shell_printf(sh, "<%s>\n", cmd);
}

static uint64_t fake_now;
static unsigned stamps;
static uint64_t now_us(void) { return fake_now; }
static void stamp(char *buf, size_t len) { snprintf(buf, len, "t%u", ++stamps); }
static const chat_hooks_t hooks = { now_us, stamp };

#define N 4
static fake_t fk[N];
static shell_t sh[N];

static void setup(void) {
    chat_init(&hooks);
    stamps = 0;
    fake_now = 0;
    for (int i = 0; i < N; i++) {
        memset(&fk[i], 0, sizeof(fk[i]));
        fk[i].space = 4096;
        shell_init(&sh[i], &io, &fk[i]);
    }
}

static void clear(int i) {
    fk[i].out_len = 0;
    fk[i].out[0] = '\0';
}

// Type into shell i (processed on the next pass).
static void type(int i, const char *s) {
    size_t n = strlen(s);
    memcpy(fk[i].in + fk[i].in_len, s, n);
    fk[i].in_len += n;
}

// A main-loop pass: every shell's input, then delivery to every shell. (On the device each
// shell's delivery follows its own poll, so a shell polled earlier gets it on the next pass.)
static void pass(void) {
    for (int i = 0; i < N; i++)
        shell_poll(&sh[i]);
    for (int i = 0; i < N; i++)
        chat_deliver(&sh[i]);
}

static bool has(int i, const char *s) {
    return strstr(fk[i].out, s) != NULL;
}

static int count(int i, const char *s) {
    int c = 0;
    for (const char *p = fk[i].out; (p = strstr(p, s)); p++)
        c++;
    return c;
}

// --- tests -----------------------------------------------------------------------------------

TEST(wall_reaches_everyone_else) {
    setup();
    CHECK(chat_register(&sh[0], 0, "console", "console"));
    CHECK(chat_register(&sh[1], 1, "alice@laptop", "ssh 10.0.0.2"));
    CHECK(chat_register(&sh[2], 2, "bob", "ssh 10.0.0.3"));
    for (int i = 0; i < N; i++)
        clear(i);
    type(1, "wall hello there\r");
    pass();
    CHECK(has(1, "sent to 2 shells"));
    CHECK(!has(1, "[wall"));
    CHECK(has(0, "[wall t1 alice@laptop] hello there\n"));
    CHECK(has(2, "[wall t1 alice@laptop] hello there\n"));
    CHECK_EQ(fk[3].out_len, 0);                 // not registered
    // Delivered once only.
    clear(0);
    pass();
    CHECK_EQ(fk[0].out_len, 0);
    // The prompt is redrawn after the message, with what was being typed.
    clear(2);
    type(2, "ls /fo");
    pass();
    type(0, "wall again\r");
    pass();
    CHECK(has(2, "\r\x1b[K[wall t2 console] again\nsos> ls /fo"));
}

TEST(wall_alone_and_usage) {
    setup();
    chat_register(&sh[0], 0, "console", "console");
    clear(0);
    type(0, "wall hi\r");
    pass();
    CHECK(has(0, "nobody else is logged in"));
    type(0, "wall\r");
    pass();
    CHECK(has(0, "usage: wall"));
    type(3, "wall hi\r");                       // unregistered shell
    pass();
    CHECK(has(3, "can't send"));
}

TEST(chat_only_reaches_the_room) {
    setup();
    chat_register(&sh[0], 0, "console", "console");
    chat_register(&sh[1], 1, "alice", "a");
    chat_register(&sh[2], 2, "bob", "b");
    type(1, "chat\r");
    pass();
    CHECK(has(1, "in the chat: alice (you)"));
    CHECK(has(1, "chat> "));
    type(2, "chat\r");
    pass();
    CHECK(has(1, "-- t2 bob joined the chat"));
    CHECK(has(2, "in the chat: alice, bob (you)"));
    for (int i = 0; i < N; i++)
        clear(i);
    type(1, "hi bob\r");
    pass();
    CHECK(has(2, "[t3] alice: hi bob\n"));
    CHECK(has(2, "\nchat> "));                  // bob's prompt redrawn
    CHECK(!has(1, "[t3]"));                     // not echoed back
    CHECK_EQ(fk[0].out_len, 0);                 // console isn't in the room
    // Walls still reach the room.
    type(0, "wall all\r");
    pass();
    CHECK(has(1, "[wall t4 console] all"));
    CHECK(has(2, "[wall t4 console] all"));
    // Leaving: Ctrl-D, Ctrl-C and /quit.
    clear(1);
    type(2, "\x04");
    pass();
    CHECK(has(2, "(left the chat)"));
    CHECK(has(1, "-- t5 bob left the chat"));
    type(2, "bob again\r");                     // back at the shell prompt
    pass();
    CHECK(has(2, "<bob>"));
    type(1, "\x03");
    pass();
    CHECK(has(1, "(left the chat)"));
    type(0, "chat\r/quit\r");
    pass();
    CHECK(has(0, "(left the chat)"));
}

TEST(chat_slash_lines) {
    setup();
    chat_register(&sh[1], 1, "alice", "a");
    chat_register(&sh[2], 2, "bob", "b");
    type(1, "chat\r");
    type(2, "chat\r");
    pass();
    clear(1);
    clear(2);
    type(1, "/who\r/nope\r//slash\r   \r\r");
    pass();
    CHECK(has(1, "in the chat: alice (you), bob"));
    CHECK(has(1, "/who lists the room"));
    CHECK(has(2, "alice: /slash\n"));
    CHECK_EQ(count(2, "alice:"), 1);            // blank lines aren't sent
}

TEST(join_shows_recent_history) {
    setup();
    chat_register(&sh[1], 1, "alice", "a");
    chat_register(&sh[2], 2, "bob", "b");
    type(1, "chat\r");
    pass();
    for (int k = 0; k < 7; k++) {
        char line[16];
        snprintf(line, sizeof(line), "line%d\r", k);
        type(1, line);
    }
    pass();
    clear(2);
    type(2, "chat\r");
    pass();
    CHECK(!has(2, "line1\n"));
    CHECK(has(2, "line2\n"));
    CHECK(has(2, "line6\n"));
    CHECK_EQ(count(2, "alice: line"), CHAT_HISTORY);
    CHECK(!has(2, "joined"));                   // alice's join isn't history
    clear(2);
    pass();
    CHECK_EQ(fk[2].out_len, 0);                 // history isn't delivered again
}

TEST(slow_client_throttled_then_missed) {
    setup();
    chat_register(&sh[1], 1, "alice", "a");
    chat_register(&sh[2], 2, "bob", "b");
    // No room on bob's channel: nothing is written, nothing lost yet.
    fk[2].space = 0;
    type(1, "wall one\r");
    pass();
    clear(2);
    pass();
    CHECK_EQ(fk[2].out_len, 0);
    // Room for one message per pass.
    type(1, "wall two\r");
    pass();
    fk[2].space = 45;                           // one line and the redraw
    pass();
    CHECK(has(2, "one"));
    CHECK(!has(2, "two"));
    pass();
    CHECK(has(2, "two"));
    // Stalled while the ring wraps: the sender waits up to CHAT_WAIT_US, then overwrites; the
    // stalled peer is told how many it missed, then gets the rest.
    fk[2].space = 0;
    clear(1);
    for (int k = 0; k < CHAT_RING + 3; k++)
        type(1, "wall more\r");
    pass();
    pass();
    CHECK(shell_job_running(&sh[1]));           // alice waits for room
    CHECK_EQ(count(1, "sent to"), CHAT_RING);
    fake_now += CHAT_WAIT_US;
    pass();
    pass();
    pass();
    CHECK(!shell_job_running(&sh[1]));
    CHECK_EQ(count(1, "sent to"), CHAT_RING + 3);
    clear(2);
    fk[2].space = 100000;
    pass();
    CHECK(has(2, "(3 messages missed)\n"));
    CHECK_EQ(count(2, "] more"), CHAT_RING);
}

TEST(burst_reaches_readers_that_keep_up) {
    setup();
    chat_register(&sh[1], 1, "alice", "a");
    chat_register(&sh[2], 2, "bob", "b");
    type(1, "chat\r");
    type(2, "chat\r");
    pass();
    clear(2);
    for (int k = 0; k < CHAT_RING * 2; k++)    // a paste, all in one read
        type(1, "line\r");
    for (int k = 0; k < 10; k++)
        pass();
    CHECK(!has(2, "missed"));
    CHECK_EQ(count(2, "alice: line"), CHAT_RING * 2);
    CHECK(!shell_job_running(&sh[1]));
    CHECK(has(1, "chat> "));
}

TEST(ctrl_c_while_waiting) {
    setup();
    chat_register(&sh[1], 1, "alice", "a");
    chat_register(&sh[2], 2, "bob", "b");
    chat_register(&sh[3], 3, "carol", "c");
    fk[2].space = 0;
    for (int k = 0; k < CHAT_RING + 1; k++)
        type(1, "wall w\r");
    pass();
    CHECK(shell_job_running(&sh[1]));
    type(1, "\x03");
    pass();
    CHECK(has(1, "wall: not sent"));
    CHECK(!shell_job_running(&sh[1]));
    // In the chat, Ctrl-C while waiting drops the line and leaves the room.
    type(3, "chat\r");
    type(2, "chat\r");
    pass();
    fake_now += CHAT_WAIT_US;                   // let the old walls go
    for (int k = 0; k < CHAT_RING + 1; k++)
        type(3, "c\r");
    pass();
    pass();
    CHECK(shell_job_running(&sh[3]));
    type(3, "\x03");
    pass();
    CHECK(has(3, "(left the chat)"));
    CHECK(has(3, "sos> "));
    // The session ending while waiting leaves the room too.
    type(1, "chat\r");
    pass();
    fake_now += CHAT_WAIT_US;
    for (int k = 0; k < CHAT_RING + 1; k++)
        type(1, "d\r");
    pass();
    pass();
    CHECK(shell_job_running(&sh[1]));
    shell_end(&sh[1]);
    chat_unregister(&sh[1]);
    CHECK(chat_name(&sh[1]) == NULL);
}

TEST(missed_counts_only_messages_for_the_peer) {
    setup();
    chat_register(&sh[1], 1, "alice", "a");
    chat_register(&sh[2], 2, "bob", "b");
    chat_register(&sh[3], 3, "carol", "c");
    type(1, "chat\r");
    type(2, "chat\r");
    pass();
    fk[3].space = 0;                            // carol isn't in the room and is stalled
    for (int k = 0; k < CHAT_RING + 4; k++)
        type(1, "x\r");
    pass();
    fk[3].space = 4096;
    clear(3);
    pass();
    CHECK_EQ(fk[3].out_len, 0);                 // chat lines were never hers
}

TEST(unregister_announces_and_frees) {
    setup();
    chat_register(&sh[1], 1, "alice", "a");
    chat_register(&sh[2], 2, "bob", "b");
    type(1, "chat\r");
    type(2, "chat\r");
    type(1, "unread\r");
    pass();
    shell_end(&sh[2]);
    chat_unregister(&sh[2]);
    CHECK(chat_name(&sh[2]) == NULL);
    clear(1);
    pass();
    CHECK(has(1, "bob left the chat"));
    // The slot is reused by a new login.
    shell_init(&sh[2], &io, &fk[2]);
    CHECK(chat_register(&sh[2], 2, "dave", "d"));
    clear(2);
    pass();
    CHECK_EQ(fk[2].out_len, 0);                 // nothing from before its login
    // Delivering to an unregistered shell does nothing.
    chat_unregister(&sh[2]);
    type(1, "later\r");
    pass();
    chat_deliver(&sh[2]);
}

static bool raw_done;
static bool raw_step(shell_t *s, void *arg, bool cancel) {
    (void)s; (void)arg;
    return !cancel && !raw_done;
}
static void raw_key(shell_t *s, int key, void *arg) {
    (void)s; (void)arg;
    if (key == 'q')
        raw_done = true;
}

TEST(nothing_delivered_in_raw_mode) {
    setup();
    chat_register(&sh[1], 1, "alice", "a");
    chat_register(&sh[2], 2, "bob", "b");
    raw_done = false;
    shell_run_raw(&sh[2], raw_step, raw_key, NULL);
    CHECK(shell_raw_mode(&sh[2]));
    clear(2);
    type(1, "wall in the editor?\r");
    pass();
    pass();
    CHECK(!has(2, "in the editor?"));
    type(2, "q");
    pass();
    pass();
    CHECK(!shell_raw_mode(&sh[2]));
    CHECK(has(2, "[wall t1 alice] in the editor?"));
}

TEST(table_full) {
    setup();
    for (int i = 0; i < N; i++)
        CHECK(chat_register(&sh[i], (unsigned)i, "x", "y"));
    static shell_t extra;
    CHECK(!chat_register(&extra, 9, "z", "z"));
}

TEST(names_unique_and_clean) {
    setup();
    chat_register(&sh[1], 1, "alice", "a");
    chat_register(&sh[2], 2, "alice", "a");
    CHECK(!strcmp(chat_name(&sh[1]), "alice"));
    CHECK(!strcmp(chat_name(&sh[2]), "alice/2"));
    chat_register(&sh[3], 3, "evil\x1b[2J\x9b" "1m\ttab", "\x1b]0;x\x07");
    CHECK(!strcmp(chat_name(&sh[3]), "evil.[2J.1m tab"));
    chat_register(&sh[0], 0, "", "console");
    CHECK(!strcmp(chat_name(&sh[0]), "?"));
    // Long names are cut on a character boundary, leaving room for a suffix.
    char lng[80];
    memset(lng, 'a', sizeof(lng));
    strcpy(lng + 42, "\xc3\xa9\xc3\xa9\xc3\xa9");    // "ééé" across the cut
    chat_unregister(&sh[3]);
    chat_register(&sh[3], 3, lng, "x");
    const char *n = chat_name(&sh[3]);
    CHECK(strlen(n) <= CHAT_NAME_MAX - 2);
    CHECK(((unsigned char)n[strlen(n) - 1] & 0xc0) != 0xc0);   // no dangling lead byte
    chat_unregister(&sh[2]);
    chat_register(&sh[2], 2, lng, "x");
    CHECK(strlen(chat_name(&sh[2])) <= CHAT_NAME_MAX);
    CHECK(strstr(chat_name(&sh[2]), "/2") != NULL);
}

TEST(who_lists_shells) {
    setup();
    fake_now = 5000000;
    chat_register(&sh[2], 2, "bob", "ssh 10.0.0.3");
    chat_register(&sh[0], 0, "console", "console");
    type(2, "chat\r");
    pass();
    fake_now = 5000000 + 3723000000ull;
    clear(0);
    type(0, "who\r");
    pass();
    char *c = strstr(fk[0].out, "* 0  console");
    char *b = strstr(fk[0].out, "  2  bob");
    CHECK(c && b && c < b);                     // slot order, own marked
    CHECK(has(0, "chat 1h02m"));
}

TEST_MAIN(
    RUN(wall_reaches_everyone_else);
    RUN(wall_alone_and_usage);
    RUN(chat_only_reaches_the_room);
    RUN(chat_slash_lines);
    RUN(join_shows_recent_history);
    RUN(slow_client_throttled_then_missed);
    RUN(burst_reaches_readers_that_keep_up);
    RUN(ctrl_c_while_waiting);
    RUN(missed_counts_only_messages_for_the_peer);
    RUN(unregister_announces_and_frees);
    RUN(nothing_delivered_in_raw_mode);
    RUN(table_full);
    RUN(names_unique_and_clean);
    RUN(who_lists_shells);
)
