// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/shell/login.c: the console's lock, with the real shell line editor
// (shell_core.c) on a fake channel and a fake password check.

#include <stdint.h>
#include <string.h>

#include "shell/complete.h"
#include "shell/login.h"
#include <stdlib.h>

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
static int auto_locks;
static bool connected;            // what main.c would pass to login_poll()
static uint32_t idle_limit;
static char auto_why[64];
static void h_auto(const char *why) { auto_locks++; snprintf(auto_why, sizeof(auto_why), "%s", why); }

static const login_hooks_t hooks = {
    h_now, h_has, h_begin, h_step, h_abort, h_no_keys, h_rescue, h_opened, h_first, h_failed, h_auto,
    NULL, NULL, NULL,                   // no count kept across reboots
};

// The count kept across reboots: `kept` stands for what is in flash. boot_kept() starts the
// console again without touching it, as a reset would.
static unsigned kept, kept_at_begin;
static int count_calls, clear_calls;
static unsigned h_kept(void) { return kept; }
static void h_count(void) { count_calls++; kept++; }
static void h_clear(void) { clear_calls++; kept = 0; }
static bool h_begin_kept(const char *pw) {
    kept_at_begin = kept;               // what a reset during this check would leave behind
    return h_begin(pw);
}
static const login_hooks_t hooks_kept = {
    h_now, h_has, h_begin_kept, h_step, h_abort, h_no_keys, h_rescue, h_opened, h_first, h_failed,
    h_auto, h_kept, h_count, h_clear,
};

// --- fake command module ----------------------------------------------------------------------------

static char executed[16][SHELL_LINE_MAX + 1];
static int n_executed;

// "job": a command that runs until cancelled.
static int job_cancels;
static bool endless_job(shell_t *s, void *arg, bool cancel) {
    (void)s; (void)arg;
    if (cancel)
        job_cancels++;
    return !cancel;
}

static const shell_cmd_t commands[] = {{"job", NULL, NULL, 0}, {"logout", NULL, NULL, 0}, {0}};
static const shell_cmd_t *const tables[] = {commands, NULL};

const shell_cmd_t *const *shell_command_tables(const shell_t *s) {   // as shell.c does
    return s->io->admin && login_locked() ? NULL : tables;
}

complete_dir_t shell_list_dir(shell_t *s, const char *dir, complete_entry_fn fn, void *arg) {
    (void)s; (void)dir; (void)fn; (void)arg;
    return COMPLETE_DIR_NONE;
}

void shell_page(shell_t *sh, const char *name, char *text, size_t len) {
    (void)sh; (void)name; (void)len;
    free(text);                             // not under test here
}

void shell_execute(shell_t *s, char *cmd) {
    if (login_gate(s, cmd))             // as shell.c does for the console
        return;
    if (!strcmp(cmd, "job")) {
        shell_run_job(s, endless_job, NULL);
        return;
    }
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
    auto_locks = 0;
    connected = true;
    idle_limit = 0;
    job_cancels = 0;
    steps_needed = 3;
    begin_fails = false;
    shell_bind(&sh, &console_io, NULL);
    login_start(&sh, &hooks);
}

static void boot_kept(const char *pw) {
    boot(pw);
    count_calls = clear_calls = 0;
    begins = steps = aborts = opens = first_uses = 0;   // boot() started the console once already
    shell_bind(&sh, &console_io, NULL);
    out_len = 0;
    out[0] = '\0';
    login_start(&sh, &hooks_kept);
}

static void type(const char *text) {
    size_t n = strlen(text);
    CHECK(in_len + n < sizeof(in));
    memcpy(in + in_len, text, n);
    in_len += n;
}

// Run the main loop for a while (each pass 5 ms, as on the device): the shell, then the
// console's own lock, as main.c does.
static bool connected;
static uint32_t idle_limit;

static void run_ms(unsigned ms) {
    for (unsigned t = 0; t < ms; t += 5) {
        size_t before = in_pos;
        shell_poll(&sh);
        if (in_pos != before)
            login_activity();
        login_poll(&sh, connected, idle_limit);
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

// Ctrl-C while a check runs drops it, and the answer stays counted with the delay it earns:
// interrupting checks is no way round the count. Ctrl-C during the wait doesn't skip the wait.
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
    CHECK_EQ(login_failures(), 1);              // counted when its check started

    steps_needed = 3;
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

// Locking wipes the command history, so whoever logs in next can't press Up to see it.
TEST(locking_wipes_the_command_history) {
    boot("open sesame");
    type("open sesame\r");
    run_ms(100);
    type("key list\r");
    run_ms(20);
    CHECK(sh.history_len > 0);
    type("logout\r");                           // `logout`
    run_ms(20);
    CHECK(login_locked());
    CHECK_EQ(sh.history_len, 0);
    type("open sesame\r");
    run_ms(1500);
    CHECK(!login_locked());
    type("\x1b[A");                             // Up: nothing to recall
    run_ms(20);
    CHECK_EQ(sh.line_len, 0);
    type("uptime\r");
    run_ms(20);
    CHECK(sh.history_len > 0);
    connected = false;                           // the terminal goes away
    run_ms(20);
    CHECK(login_locked());
    CHECK_EQ(sh.history_len, 0);
}

TEST(logout_without_a_password_does_not_lock) {
    boot(NULL);
    type("logout\r");
    run_ms(20);
    CHECK(!login_locked());
    CHECK(shown("no console password is set"));
}

// Lines refused by the gate while locked (the question dropped) aren't offered by Up after login.
TEST(lines_typed_while_locked_are_not_in_the_history) {
    boot("open sesame");
    shell_cancel_question(&sh);                 // as if some code dropped it
    type("a password guess\r");
    run_ms(20);
    CHECK_EQ(n_executed, 0);
    CHECK(login_locked());
    type("open sesame\r");                      // the gate asked again
    run_ms(1500);
    CHECK(!login_locked());
    CHECK_EQ(sh.history_len, 0);
    type("\x1b[A");
    run_ms(20);
    CHECK_EQ(sh.line_len, 0);
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

// Tab offers nothing before the password: not at the prompt, not with the question dropped,
// and it echoes nothing there.
TEST(tab_completes_nothing_while_locked) {
    boot("open sesame");
    clear_out();
    type("jo\t\t");
    run_ms(20);
    CHECK(!strcmp(out, ""));
    type("\x03");                               // drop "jo"
    run_ms(20);
    shell_cancel_question(&sh);                 // as if some code dropped it
    clear_out();
    type("jo\t\t");
    run_ms(20);
    CHECK(!strcmp(out, "jo"));                  // echoed, but no completion, not even a bell
    type("\x03" "x\r");                         // a line brings the question back
    run_ms(20);
    CHECK(login_locked());
    type("open sesame\r");
    run_ms(100);
    CHECK(!login_locked());
    clear_out();
    type("jo\t");                               // open: Tab works
    run_ms(20);
    CHECK(!strcmp(out, "job "));
}

// --- the console locking itself ----------------------------------------------------------------------

static void open_console(void) {
    boot("open sesame");
    type("open sesame\r");
    run_ms(100);
    CHECK(!login_locked());
    clear_out();
}

TEST(disconnecting_the_terminal_locks) {
    open_console();
    run_ms(1000);
    CHECK(!login_locked());                     // connected and no idle limit: stays open
    connected = false;
    run_ms(10);
    CHECK(login_locked());
    CHECK_EQ(auto_locks, 1);
    CHECK(strstr(auto_why, "disconnected") != NULL);
    // The next terminal finds the password prompt, and commands don't run.
    connected = true;
    type("config\r");
    run_ms(100);
    CHECK_EQ(n_executed, 0);
    type("open sesame\r");
    run_ms(1500);
    CHECK(!login_locked());
    CHECK_EQ(auto_locks, 1);                    // reconnecting doesn't lock again
}

TEST(idle_console_locks) {
    open_console();
    idle_limit = 60000;
    run_ms(59000);
    CHECK(!login_locked());
    run_ms(1500);
    CHECK(login_locked());
    CHECK(!strcmp(auto_why, "idle"));
    CHECK(shown("console locked (idle)"));
    CHECK(shown("Password: "));
}

TEST(typing_restarts_the_idle_time) {
    open_console();
    idle_limit = 60000;
    for (int i = 0; i < 5; i++) {
        run_ms(50000);
        type("x");                              // not even a whole command
    }
    CHECK(!login_locked());
    run_ms(61000);
    CHECK(login_locked());
}

// What was typed and not entered doesn't become part of the next person's first answer, and a
// running command is ended.
TEST(auto_lock_discards_input_and_ends_a_job) {
    open_console();
    type("key add ssh-ed25519 AAAA");           // half a command line
    run_ms(20);
    connected = false;
    run_ms(10);
    CHECK(login_locked());
    CHECK_EQ(sh.line_len, 0);
    for (size_t i = 0; i < sizeof(sh.line); i++)
        CHECK_EQ(sh.line[i], 0);
    connected = true;
    type("open sesame\r");
    run_ms(200);
    CHECK(!login_locked());                     // the password alone was the answer

    type("job\r");
    run_ms(20);
    CHECK(shell_job_running(&sh));
    idle_limit = 1000;
    run_ms(1200);
    CHECK(login_locked());
    CHECK_EQ(job_cancels, 1);
    CHECK(!shell_job_running(&sh));
    idle_limit = 0;
    type("open sesame\r");
    run_ms(200);
    CHECK(!login_locked());
}

// A pending question (say, the middle of `setup`) is dropped too: its answer would otherwise be
// taken from the next person.
static int answers;
static void on_answer(shell_t *s, char *answer, void *arg) { (void)s; (void)answer; (void)arg; answers++; }

TEST(auto_lock_drops_a_pending_question) {
    open_console();
    answers = 0;
    shell_ask(&sh, "Wi-Fi network: ", false, on_answer, NULL);
    connected = false;
    run_ms(10);
    connected = true;
    type("open sesame\r");
    run_ms(200);
    CHECK_EQ(answers, 0);
    CHECK(!login_locked());
}

// Nothing to lock with: no password set. And a locked console isn't locked again.
TEST(no_auto_lock_without_a_password_or_when_locked) {
    boot(NULL);
    idle_limit = 1000;
    run_ms(3000);
    connected = false;
    run_ms(10);
    CHECK(!login_locked());
    CHECK_EQ(auto_locks, 0);
    boot("open sesame");
    idle_limit = 1000;
    clear_out();
    run_ms(5000);
    connected = false;
    run_ms(10);
    CHECK(login_locked());
    CHECK_EQ(auto_locks, 0);
    CHECK(!shown("console locked"));
}

// --- the count kept across reboots -------------------------------------------------------------------

// An answer is counted, durably, before its check begins.
TEST(kept_count_is_written_before_the_check) {
    kept = 0;
    boot_kept("open sesame");
    CHECK_EQ(login_failures(), 0);
    type("wrong one\r");
    run_ms(100);
    CHECK_EQ(begins, 1);
    CHECK_EQ(kept_at_begin, 1);                 // already there when the check started
    CHECK_EQ(kept, 1);
    CHECK_EQ(login_failures(), 1);
    CHECK_EQ(count_calls, 1);                   // once per answer, not again when it is wrong
}

// The reason for it: a reset while the check runs (the power pulled before "wrong password")
// leaves the answer counted, and the next boot starts with the wait it earned.
TEST(reset_during_a_check_counts_and_delays) {
    kept = 0;
    boot_kept("open sesame");
    steps_needed = 50;
    type("guess 1\r");
    run_ms(20);                                 // under way, undecided
    CHECK_EQ(begins, 1);
    boot_kept("open sesame");                   // reset
    CHECK_EQ(login_failures(), 1);
    type("open sesame\r");
    run_ms(900);
    CHECK_EQ(begins, 0);                        // the 1 s wait holds across the reset
    CHECK(login_locked());
    run_ms(300);
    CHECK(!login_locked());
    CHECK_EQ(kept, 0);                          // a right answer clears what was kept
    CHECK_EQ(clear_calls, 1);
}

// Wrong answers, then a reset: the delays carry on from where they were, they don't start over.
TEST(reset_does_not_restart_the_delays) {
    kept = 0;
    boot_kept("open sesame");
    type("a\r");
    run_ms(100);
    type("b\r");
    run_ms(1200);
    type("c\r");
    run_ms(2200);
    CHECK_EQ(login_failures(), 3);
    CHECK_EQ(kept, 3);
    boot_kept("open sesame");                   // reset
    CHECK_EQ(login_failures(), 3);
    type("d\r");
    run_ms(3900);
    CHECK_EQ(begins, 0);                        // 4 s, the third failure's delay, from boot
    run_ms(300);
    CHECK_EQ(begins, 1);
    CHECK_EQ(kept, 4);
    CHECK_EQ(login_failures(), 4);
    type("open sesame\r");
    run_ms(7500);
    CHECK(login_locked());                      // 8 s after the fourth
    run_ms(700);
    CHECK(!login_locked());
    CHECK_EQ(kept, 0);
    boot_kept("open sesame");                   // and a reset after a right answer starts clean
    CHECK_EQ(login_failures(), 0);
    type("open sesame\r");
    run_ms(100);
    CHECK(!login_locked());
}

// A kept count far beyond the cap waits the capped time, no longer.
TEST(kept_count_delay_is_capped) {
    kept = 5000;
    boot_kept("open sesame");
    type("open sesame\r");
    run_ms(29900);
    CHECK(login_locked());
    run_ms(300);
    CHECK(!login_locked());
    CHECK_EQ(kept, 0);
}

// Ctrl-C during a check: counted and kept. An answer too long to be a password, and a check
// that can't start: counted and kept too.
TEST(kept_count_includes_interrupted_and_unchecked_answers) {
    kept = 0;
    boot_kept("open sesame");
    steps_needed = 50;
    type("guess\r");
    run_ms(20);
    type("\x03");
    run_ms(20);
    CHECK_EQ(kept, 1);
    steps_needed = 3;
    char longpw[LOGIN_PASSWORD_MAX + 3];
    memset(longpw, 'x', LOGIN_PASSWORD_MAX + 1);
    longpw[LOGIN_PASSWORD_MAX + 1] = '\r';
    longpw[LOGIN_PASSWORD_MAX + 2] = '\0';
    type(longpw);
    run_ms(1200);
    CHECK_EQ(kept, 2);
    begin_fails = true;
    type("anything\r");
    run_ms(2200);
    CHECK_EQ(kept, 3);
    CHECK_EQ(login_failures(), 3);
}

// Waiting, Ctrl-C during the wait, an empty line and Ctrl-D count nothing: no check started.
TEST(kept_count_ignores_what_never_reached_a_check) {
    kept = 2;
    boot_kept("open sesame");
    type("\r");
    type("\x03");
    type("\x04");
    run_ms(100);
    type("guess\r");
    run_ms(500);                                // inside the 2 s wait
    type("\x03");
    run_ms(50);
    CHECK_EQ(kept, 2);
    CHECK_EQ(count_calls, 0);
    CHECK_EQ(begins, 0);
}

// No password set: the console opens without a check, and a kept count changes nothing.
TEST(kept_count_without_a_password) {
    kept = 4;
    boot_kept(NULL);
    CHECK(!login_locked());
    CHECK_EQ(first_uses, 1);
    CHECK_EQ(kept, 4);
}

// A board that boots with a kept count and no password (the configuration was erased, say):
// the console is open, a password is set, the console is locked. The old count must not make
// the first wrong answer against the new password wait as if it were the fifth.
TEST(setting_a_password_starts_the_count_again) {
    kept = 4;
    boot_kept(NULL);                            // no password: open, 4 wrong answers kept
    CHECK(!login_locked());
    CHECK_EQ(login_failures(), 4);
    password = "new password";                  // `passwd` at the open console
    login_password_set();
    CHECK_EQ(login_failures(), 0);
    CHECK_EQ(kept, 0);                          // and in what is kept across reboots
    CHECK_EQ(clear_calls, 1);
    type("logout\r");
    run_ms(50);
    CHECK(login_locked());
    type("wrong\r");
    run_ms(100);
    CHECK_EQ(login_failures(), 1);
    type("new password\r");
    run_ms(900);
    CHECK(login_locked());                      // the first wrong answer's 1 s, no more
    run_ms(300);
    CHECK(!login_locked());
    // Without a kept count (the hooks absent) it is harmless.
    boot("pw");
    login_password_set();
    CHECK_EQ(login_failures(), 0);
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
    RUN(locking_wipes_the_command_history);
    RUN(lines_typed_while_locked_are_not_in_the_history);
    RUN(logout_without_a_password_does_not_lock);
    RUN(gate_holds_without_the_question);
    RUN(no_keys_locks_with_rescue_commands_only);
    RUN(tab_completes_nothing_while_locked);
    RUN(disconnecting_the_terminal_locks);
    RUN(idle_console_locks);
    RUN(typing_restarts_the_idle_time);
    RUN(auto_lock_discards_input_and_ends_a_job);
    RUN(auto_lock_drops_a_pending_question);
    RUN(no_auto_lock_without_a_password_or_when_locked);
    RUN(kept_count_is_written_before_the_check);
    RUN(reset_during_a_check_counts_and_delays);
    RUN(reset_does_not_restart_the_delays);
    RUN(kept_count_delay_is_capped);
    RUN(kept_count_includes_interrupted_and_unchecked_answers);
    RUN(kept_count_ignores_what_never_reached_a_check);
    RUN(kept_count_without_a_password);
    RUN(setting_a_password_starts_the_count_again);
)
