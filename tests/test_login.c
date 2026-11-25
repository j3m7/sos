// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/shell/login.c: the console's lock, with the real shell line editor
// (shell_core.c) on a fake channel and a fake password check.

#include <stdint.h>
#include <string.h>

#include "shell/login.h"
#include "test.h"

// --- fake console channel --------------------------------------------------------------------------

static char in[1024], out[16384];
static size_t in_len, in_pos, out_len;

static int f_read(void *ctx, char *buf, int len) {
    (void)ctx;
    size_t n = in_len - in_pos;
    if (n > (size_t)len)
        n = (size_t)len;
    memcpy(buf, in + in_pos, n);
    in_pos += n;
    return (int)n;
}

static void f_write(void *ctx, const char *buf, int len) {
    (void)ctx;
    CHECK(out_len + (size_t)len < sizeof(out));
    memcpy(out + out_len, buf, (size_t)len);
    out_len += (size_t)len;
    out[out_len] = '\0';
}

static shell_t sh;
static void f_logout(void *ctx) { (void)ctx; login_lock(&sh); }

static const shell_io_t console_io = {
    .name = "console", .read = f_read, .write = f_write, .logout = f_logout, .admin = true,
};

// --- fake password check ----------------------------------------------------------------------------

static uint64_t now;
static const char *password;            // NULL: none set
static const char *no_keys_why;
static int begins, steps, aborts, opens, first_uses;
static int steps_needed;                // slices a check takes
static bool begin_fails;
static char checked[LOGIN_PASSWORD_MAX + 1];
static int steps_left;
static const char *begun_with;          // the buffer check_begin was given (to see it wiped)

static uint64_t h_now(void) { return now; }
static bool h_has(void) { return password != NULL; }
static bool h_begin(const char *pw) {
    begins++;
    if (begin_fails)
        return false;
    begun_with = pw;
    strcpy(checked, pw);
    steps_left = steps_needed;
    return true;
}
static int h_step(void) {
    steps++;
    if (--steps_left > 0)
        return LOGIN_MORE;
    return password && !strcmp(checked, password) ? LOGIN_RIGHT : LOGIN_WRONG;
}
static void h_abort(void) { aborts++; }
static const char *h_no_keys(void) { return no_keys_why; }
static bool h_rescue(const char *cmd) {
    return !strcmp(cmd, "rootsecret") || !strcmp(cmd, "rootsecret forget") || !strcmp(cmd, "reboot");
}
static void h_opened(shell_t *s) { opens++; shell_greet(s); }
static void h_first(shell_t *s) { first_uses++; shell_printf(s, "set a password first\n"); }
static unsigned failed_calls, failed_last;
static void h_failed(unsigned n) { failed_calls++; failed_last = n; }

static const login_hooks_t hooks = {
    h_now, h_has, h_begin, h_step, h_abort, h_no_keys, h_rescue, h_opened, h_first, h_failed,
};

// --- fake command module ----------------------------------------------------------------------------

static char executed[16][SHELL_LINE_MAX + 1];
static int n_executed;

void shell_execute(shell_t *s, char *cmd) {
    if (login_gate(s, cmd))             // as shell.c does for the console
        return;
    if (n_executed < 16)
        strcpy(executed[n_executed], cmd);
    n_executed++;
    if (!strcmp(cmd, "logout"))
        shell_logout(s);
    else
        shell_printf(s, "ran %s\n", cmd);
}

// --- helpers -----------------------------------------------------------------------------------------

static void boot(const char *pw) {
    memset(&sh, 0, sizeof(sh));
    in_len = in_pos = out_len = 0;
    out[0] = '\0';
    now = 1000;
    password = pw;
    no_keys_why = NULL;
    begins = steps = aborts = opens = first_uses = n_executed = 0;
    failed_calls = failed_last = 0;
    steps_needed = 3;
    begin_fails = false;
    shell_bind(&sh, &console_io, NULL);
    login_start(&sh, &hooks);
}

static void type(const char *text) {
    size_t n = strlen(text);
    CHECK(in_len + n < sizeof(in));
    memcpy(in + in_len, text, n);
    in_len += n;
}

// Run the main loop for a while (each pass 5 ms, as on the device).
static void run_ms(unsigned ms) {
    for (unsigned t = 0; t < ms; t += 5) {
        shell_poll(&sh);
        now += 5;
    }
}

static bool shown(const char *text) {
    return strstr(out, text) != NULL;
}

static void clear_out(void) {
    out_len = 0;
    out[0] = '\0';
}

// --- tests -------------------------------------------------------------------------------------------

// A locked console shows a password prompt and nothing else: no greeting, no version.
TEST(locked_console_shows_only_the_prompt) {
    boot("open sesame");
    CHECK(login_locked());
    CHECK(!strcmp(out, "\nPassword: "));
    CHECK(!shown("SOS"));
}

TEST(right_password_opens_and_greets) {
    boot("open sesame");
    type("open sesame\r");
    run_ms(100);
    CHECK(!login_locked());
    CHECK_EQ(opens, 1);
    CHECK_EQ(begins, 1);
    CHECK_EQ(steps, 3);                         // the check ran in slices
    CHECK(shown("Type 'help'"));
    CHECK(!shown("open sesame"));               // not echoed
    CHECK(!shown("wrong"));
    // Commands run from here on.
    type("uptime\r");
    run_ms(20);
    CHECK_EQ(n_executed, 1);
    CHECK(!strcmp(executed[0], "uptime"));
}

TEST(nothing_runs_while_locked) {
    boot("open sesame");
    type("help\rconfig\rkey list\r");
    run_ms(8000);
    CHECK(login_locked());
    CHECK_EQ(n_executed, 0);                    // they were taken as (wrong) passwords
    CHECK(!shown("ran "));
    CHECK(!shown("SOS"));
    CHECK_EQ(login_failures(), 3);
    CHECK_EQ(failed_calls, 3);                  // each was reported for the log
    CHECK_EQ(failed_last, 3);
}

// Type an answer and run until it has been checked or `limit_ms` passed; returns how long the
// check took to start, from now.
static unsigned answer_and_wait(const char *text, unsigned limit_ms) {
    int before = begins;
    unsigned waited = 0;
    type(text);
    while (begins == before && waited < limit_ms) {
        run_ms(5);
        waited += 5;
    }
    run_ms(50);                                 // let the check finish
    return waited;
}

TEST(wrong_password_asks_again_after_a_growing_delay) {
    boot("open sesame");
    CHECK(answer_and_wait("guess one\r", 5000) < 50);       // the first is checked at once
    CHECK(login_locked());
    CHECK(shown("wrong password"));
    CHECK_EQ(login_failures(), 1);

    // After a failure the next answer isn't checked for 1 s, then 2 s, then 4 s. (Each wait is
    // measured from when the answer is typed, 50 ms after the failure.)
    unsigned w = answer_and_wait("guess two\r", 5000);
    CHECK(w >= 900 && w <= 1000);
    CHECK_EQ(login_failures(), 2);
    w = answer_and_wait("guess three\r", 5000);
    CHECK(w >= 1900 && w <= 2000);
    CHECK_EQ(login_failures(), 3);

    // The right password has to wait out the delay too, and then opens.
    w = answer_and_wait("open sesame\r", 10000);
    CHECK(w >= 3900 && w <= 4000);
    CHECK(!login_locked());
    CHECK_EQ(login_failures(), 0);              // a good login clears the count
}

TEST(delay_is_capped) {
    boot("open sesame");
    static const unsigned want[] = { 0, 1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000 };
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        unsigned w = answer_and_wait("nope nope\r", 60000);
        CHECK(w + 100 >= want[i] && w <= want[i] + 50);
    }
    CHECK_EQ(login_failures(), 9);
    unsigned w = answer_and_wait("open sesame\r", 60000);
    CHECK(w >= 29900 && w <= 30050);
    CHECK(!login_locked());
}

// Ctrl-C at the prompt, an empty line, and Ctrl-D don't count as attempts and don't open.
TEST(ctrl_c_empty_and_ctrl_d_just_ask_again) {
    boot("open sesame");
    type("\x03");
    type("\r");
    type("abc\x03");
    type("\x04");
    run_ms(50);
    CHECK(login_locked());
    CHECK_EQ(begins, 0);
    CHECK_EQ(login_failures(), 0);
    CHECK_EQ(n_executed, 0);
    clear_out();
    type("open sesame\r");
    run_ms(100);
    CHECK(!login_locked());
}

// Ctrl-C while a check runs drops it; it doesn't skip the delay of an earlier failure.
TEST(ctrl_c_during_the_check_or_the_delay) {
    boot("open sesame");
    steps_needed = 50;
    type("open sesame\r");
    run_ms(20);                                 // the check is under way
    CHECK_EQ(begins, 1);
    type("\x03");
    run_ms(20);
    CHECK_EQ(aborts, 1);
    CHECK(login_locked());
    CHECK_EQ(login_failures(), 0);

    steps_needed = 3;
    type("wrong one\r");
    run_ms(100);
    CHECK_EQ(login_failures(), 1);
    int before = begins;
    type("open sesame\r");
    run_ms(200);                                // waiting out the 1 s delay
    type("\x03");                               // cancel the wait
    run_ms(20);
    type("open sesame\r");                      // and try again at once
    run_ms(300);
    CHECK_EQ(begins, before);                   // still not checked: the delay holds
    CHECK(login_locked());
    run_ms(700);
    CHECK(!login_locked());
}

// The typed password is wiped from the line buffer and from the pending copy once checked.
TEST(password_is_wiped_after_use) {
    boot("open sesame");
    type("hunter2 hunter2\r");
    run_ms(100);
    CHECK(strstr(sh.line, "hunter2") == NULL);
    CHECK_EQ(begun_with[0], '\0');              // login.c's pending copy
    for (size_t i = 0; i + 7 <= sizeof(sh.line); i++)
        CHECK(memcmp(sh.line + i, "hunter2", 7) != 0);
}

TEST(overlong_answer_is_wrong_without_a_check) {
    boot("open sesame");
    char longpw[LOGIN_PASSWORD_MAX + 12];
    memset(longpw, 'a', sizeof(longpw));
    longpw[sizeof(longpw) - 2] = '\r';
    longpw[sizeof(longpw) - 1] = '\0';
    type(longpw);
    run_ms(100);
    CHECK_EQ(begins, 0);
    CHECK_EQ(login_failures(), 1);
    CHECK(login_locked());
}

TEST(check_that_cannot_start_counts_as_wrong) {
    boot("open sesame");
    begin_fails = true;
    type("open sesame\r");
    run_ms(100);
    CHECK(login_locked());
    CHECK_EQ(login_failures(), 1);
}

// No password set: the console is open, and the first-use hook runs (it starts setup).
TEST(no_password_opens_into_first_use) {
    boot(NULL);
    CHECK(!login_locked());
    CHECK_EQ(first_uses, 1);
    CHECK_EQ(opens, 0);
    CHECK(shown("set a password first"));
    type("uptime\r");
    run_ms(20);
    CHECK_EQ(n_executed, 1);
}

TEST(logout_locks_the_console_again) {
    boot("open sesame");
    type("open sesame\r");
    run_ms(100);
    CHECK(!login_locked());
    clear_out();
    type("logout\r");
    run_ms(20);
    CHECK(login_locked());
    CHECK(shown("console locked"));
    CHECK(shown("Password: "));
    type("uptime\r");
    run_ms(100);
    CHECK_EQ(n_executed, 1);                    // only the logout ran
    type("open sesame\r");
    run_ms(1500);
    CHECK(!login_locked());
    // Ctrl-D on an empty line is a logout too.
    type("\x04");
    run_ms(20);
    CHECK(login_locked());
}

TEST(logout_without_a_password_does_not_lock) {
    boot(NULL);
    type("logout\r");
    run_ms(20);
    CHECK(!login_locked());
    CHECK(shown("no console password is set"));
}

// The gate refuses a command line even if the password question isn't pending any more.
TEST(gate_holds_without_the_question) {
    boot("open sesame");
    shell_cancel_question(&sh);                 // as if some code dropped it
    type("config\r");
    run_ms(20);
    CHECK_EQ(n_executed, 0);
    CHECK(login_locked());
    type("open sesame\r");                      // the question is back
    run_ms(100);
    CHECK(!login_locked());
}

// No usable root secret: locked whatever the configuration says, with only the rescue commands.
TEST(no_keys_locks_with_rescue_commands_only) {
    memset(&sh, 0, sizeof(sh));
    in_len = in_pos = out_len = 0;
    now = 1000;
    password = NULL;                            // even with no password set
    no_keys_why = "the root secret can't be used";
    begins = opens = first_uses = n_executed = 0;
    shell_bind(&sh, &console_io, NULL);
    login_start(&sh, &hooks);
    CHECK(login_locked());
    CHECK_EQ(first_uses, 0);
    CHECK(shown("The console is locked: the root secret can't be used"));
    CHECK(!shown("Type 'help'"));

    type("config\rsetup\rkey add x\r");
    run_ms(50);
    CHECK_EQ(n_executed, 0);
    type("rootsecret\r");
    run_ms(20);
    CHECK_EQ(n_executed, 1);
    CHECK(!strcmp(executed[0], "rootsecret"));
    type("rootsecret forget\rreboot\r");
    run_ms(20);
    CHECK_EQ(n_executed, 3);
    type("rootsecret forget now\r");            // not exactly a rescue command
    run_ms(20);
    CHECK_EQ(n_executed, 3);
    CHECK(login_locked());
    CHECK_EQ(begins, 0);
    type("logout\r");
    run_ms(20);
    CHECK(login_locked());
}

TEST_MAIN(
    RUN(locked_console_shows_only_the_prompt);
    RUN(right_password_opens_and_greets);
    RUN(nothing_runs_while_locked);
    RUN(wrong_password_asks_again_after_a_growing_delay);
    RUN(delay_is_capped);
    RUN(ctrl_c_empty_and_ctrl_d_just_ask_again);
    RUN(ctrl_c_during_the_check_or_the_delay);
    RUN(password_is_wiped_after_use);
    RUN(overlong_answer_is_wrong_without_a_check);
    RUN(check_that_cannot_start_counts_as_wrong);
    RUN(no_password_opens_into_first_use);
    RUN(logout_locks_the_console_again);
    RUN(logout_without_a_password_does_not_lock);
    RUN(gate_holds_without_the_question);
    RUN(no_keys_locks_with_rescue_commands_only);
)
