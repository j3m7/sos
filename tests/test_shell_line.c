// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/shell/shell_core.c: shell line editing and output.

#include <stdint.h>

#include "shell/shell.h"
#include "test.h"

// --- fake channel ----------------------------------------------------------------------------

typedef struct {
    const char *in;
    size_t in_len, in_pos;
    size_t chunk;               // bytes delivered per read() (0 = as many as asked for)
    char out[8192];
    size_t out_len;
    int logouts;
} fake_t;

static int fake_read(void *ctx, char *buf, int len) {
    fake_t *f = ctx;
    size_t n = f->in_len - f->in_pos;
    if (n > (size_t)len)
        n = (size_t)len;
    if (f->chunk && n > f->chunk)
        n = f->chunk;
    memcpy(buf, f->in + f->in_pos, n);
    f->in_pos += n;
    return (int)n;
}

static void fake_write(void *ctx, const char *buf, int len) {
    fake_t *f = ctx;
    CHECK(f->out_len + (size_t)len < sizeof(f->out));
    memcpy(f->out + f->out_len, buf, (size_t)len);
    f->out_len += (size_t)len;
    f->out[f->out_len] = '\0';
}

static void fake_logout(void *ctx) {
    ((fake_t *)ctx)->logouts++;
}

static const shell_io_t io_plain = {        // like the USB console, but with a logout
    .name = "plain", .read = fake_read, .write = fake_write, .logout = fake_logout, .crlf = false,
};
static const shell_io_t io_crlf = {         // like an SSH session
    .name = "ssh", .read = fake_read, .write = fake_write, .logout = fake_logout, .crlf = true,
};
static const shell_io_t io_console = {      // can't be logged out of
    .name = "console", .read = fake_read, .write = fake_write, .logout = NULL, .crlf = false,
};

// --- fake command module ---------------------------------------------------------------------

static char executed[16][SHELL_LINE_MAX + 1];
static int n_executed;

// "job": starts a job that prints "step N" for job_steps steps (without a final newline when
// job_partial), then finishes. Records cancels.
static int job_steps, job_calls, job_cancels;
static bool job_partial;
static int job_arg_token;

static bool fake_job(shell_t *sh, void *arg, bool cancel) {
    CHECK(arg == &job_arg_token);
    job_calls++;
    if (cancel) {
        job_cancels++;
        return false;
    }
    shell_printf(sh, job_partial ? "step %d" : "step %d\n", job_calls);
    return job_calls < job_steps;
}

// "raw": starts a raw-mode job that records its keys and finishes after Ctrl-Q, on its next step.
static int raw_keys_got[64], n_raw_keys, raw_steps, raw_cancels;
static bool raw_quit;

static void fake_raw_key(shell_t *sh, int key, void *arg) {
    (void)sh;
    CHECK(arg == &job_arg_token);
    if (n_raw_keys < 64)
        raw_keys_got[n_raw_keys] = key;
    n_raw_keys++;
    if (key == 0x11)
        raw_quit = true;
}

static bool fake_raw_step(shell_t *sh, void *arg, bool cancel) {
    (void)sh;
    CHECK(arg == &job_arg_token);
    raw_steps++;
    if (cancel) {
        raw_cancels++;
        return false;
    }
    return !raw_quit;
}

void shell_execute(shell_t *sh, char *cmd) {
    if (n_executed < 16)
        strcpy(executed[n_executed], cmd);
    n_executed++;
    if (!strcmp(cmd, "job")) {
        job_calls = job_cancels = 0;
        shell_run_job(sh, fake_job, &job_arg_token);
        return;
    }
    if (!strcmp(cmd, "raw")) {
        n_raw_keys = raw_steps = raw_cancels = 0;
        raw_quit = false;
        shell_run_raw(sh, fake_raw_step, fake_raw_key, &job_arg_token);
        return;
    }
    shell_printf(sh, "<%s>\n", cmd);
}

// --- helpers ---------------------------------------------------------------------------------

static fake_t fake;
static shell_t sh;

// Fresh shell on the given channel; output from shell_init() is discarded.
static void start(const shell_io_t *io) {
    memset(&fake, 0, sizeof(fake));
    n_executed = 0;
    shell_init(&sh, io, &fake);
    fake.out_len = 0;
    fake.out[0] = '\0';
}

// Deliver input (may contain NULs, hence the explicit length) and poll once.
static void type_n(const char *s, size_t n) {
    fake.in = s;
    fake.in_len = n;
    fake.in_pos = 0;
    shell_poll(&sh);
    CHECK_EQ(fake.in_pos, n);   // shell_poll() drains everything available
}
#define type(s) type_n((s), sizeof(s) - 1)

// Deliver input and poll once, without requiring it all to be read.
static void feed(const char *s) {
    fake.in = s;
    fake.in_len = strlen(s);
    fake.in_pos = 0;
    shell_poll(&sh);
}

static void clear_out(void) {
    fake.out_len = 0;
    fake.out[0] = '\0';
}

static int fake_space = -1;
static int fake_write_space(void *ctx) { (void)ctx; return fake_space; }
static const shell_io_t io_space = {
    .name = "ssh", .read = fake_read, .write = fake_write, .logout = fake_logout, .crlf = false,
    .write_space = fake_write_space,
};

#define CHECK_OUT(expected) do {                                                            \
        test_checks++;                                                                      \
        if (strcmp(fake.out, (expected)) != 0) {                                            \
            test_failures++;                                                                \
            fprintf(stderr, "%s:%d: %s: output\n  got:      \"%s\"\n  expected: \"%s\"\n",   \
                    __FILE__, __LINE__, test_current, fake.out, (expected));                \
        }                                                                                   \
    } while (0)

// --- tests -----------------------------------------------------------------------------------

TEST(init_prints_greeting_and_prompt) {
    memset(&fake, 0, sizeof(fake));
    shell_init(&sh, &io_plain, &fake);
    CHECK_OUT("\nSOS 0123456789ab\nType 'help' for commands.\nsos> ");
    CHECK_EQ(sh.line_len, 0);
}

TEST(command_is_echoed_and_executed) {
    start(&io_plain);
    type("help\r");
    CHECK_OUT("help\n<help>\nsos> ");
    CHECK_EQ(n_executed, 1);
    CHECK(!strcmp(executed[0], "help"));
}

TEST(line_endings) {
    start(&io_plain);
    type("a\r\nb\nc\r");                   // CRLF, LF and CR each end one line
    CHECK_EQ(n_executed, 3);
    CHECK(!strcmp(executed[0], "a") && !strcmp(executed[1], "b") && !strcmp(executed[2], "c"));
    CHECK_OUT("a\n<a>\nsos> b\n<b>\nsos> c\n<c>\nsos> ");
}

TEST(crlf_split_across_reads_counts_once) {
    start(&io_plain);
    type("x\r");
    type("\n");                             // LF of the same CRLF, arriving later
    CHECK_EQ(n_executed, 1);
    CHECK_OUT("x\n<x>\nsos> ");
}

TEST(empty_lines_prompt_without_executing) {
    start(&io_plain);
    type("\r\r\n\n");                       // CR, CRLF, LF: three empty lines
    CHECK_EQ(n_executed, 0);
    CHECK_OUT("\nsos> \nsos> \nsos> ");
}

TEST(lf_then_cr_is_two_lines) {
    start(&io_plain);
    type("\n\r");
    CHECK_OUT("\nsos> \nsos> ");
}

TEST(spaces_are_trimmed) {
    start(&io_plain);
    type("   mem  \r");
    CHECK_EQ(n_executed, 1);
    CHECK(!strcmp(executed[0], "mem"));
    type("     \r");                        // only spaces: nothing to run
    CHECK_EQ(n_executed, 1);
}

TEST(backspace_and_delete_edit_the_line) {
    start(&io_plain);
    type("ab\x7f" "c\bd\r");                // DEL removes b, BS removes c
    CHECK_EQ(n_executed, 1);
    CHECK(!strcmp(executed[0], "ad"));
    CHECK_OUT("ab\b \bc\b \bd\n<ad>\nsos> ");
}

TEST(backspace_on_empty_line_does_nothing) {
    start(&io_plain);
    type("\x7f\b\x7f");
    CHECK_OUT("");
    CHECK_EQ(sh.line_len, 0);
}

TEST(ctrl_c_discards_the_line) {
    start(&io_plain);
    type("abc\x03");
    CHECK_EQ(n_executed, 0);
    CHECK_OUT("abc^C\nsos> ");
    type("d\r");
    CHECK(!strcmp(executed[0], "d"));
}

TEST(ctrl_d_on_empty_line_logs_out) {
    start(&io_plain);
    type("\x04");
    CHECK_EQ(fake.logouts, 1);
    CHECK_OUT("\nsos> ");
}

TEST(ctrl_d_with_text_is_ignored) {
    start(&io_plain);
    type("ab\x04" "c\r");
    CHECK_EQ(fake.logouts, 0);
    CHECK(!strcmp(executed[0], "abc"));
}

TEST(ctrl_d_on_console_explains) {
    start(&io_console);
    type("\x04");
    CHECK_OUT("\nlogout: the console session can't be ended\nsos> ");
}

TEST(logout_uses_channel) {
    start(&io_plain);
    shell_logout(&sh);
    CHECK_EQ(fake.logouts, 1);
    CHECK_OUT("");
}

TEST(line_length_is_limited) {
    char in[SHELL_LINE_MAX + 21], want[SHELL_LINE_MAX + 1];
    memset(in, 'x', SHELL_LINE_MAX + 20);
    in[SHELL_LINE_MAX + 20] = '\r';
    memset(want, 'x', SHELL_LINE_MAX);
    want[SHELL_LINE_MAX] = '\0';
    start(&io_plain);
    type_n(in, sizeof(in));
    CHECK_EQ(n_executed, 1);
    CHECK(!strcmp(executed[0], want));                      // first SHELL_LINE_MAX characters
    CHECK(!strncmp(fake.out, want, SHELL_LINE_MAX));        // extra characters not echoed
    CHECK(fake.out[SHELL_LINE_MAX] == '\n');
}

TEST(control_and_high_bytes_are_ignored) {
    static const char in[] = "a\x01" "b\0c\x02" "d\x80" "e\xff" "f\x7f\x7f" "g\t\r";
    start(&io_plain);
    type_n(in, sizeof(in) - 1);
    CHECK_EQ(n_executed, 1);
    CHECK(!strcmp(executed[0], "abcdg"));                   // DEL DEL removed e and f
}

TEST(byte_at_a_time_matches_bulk) {
    static const char in[] = "  he\x7flp \x1b[A\r\nfoo\x03" "b\x1bOPar\x1b[1;5C\r\n\r";
    char bulk[sizeof(fake.out)];
    start(&io_plain);
    type(in);
    strcpy(bulk, fake.out);
    int bulk_n = n_executed;
    start(&io_plain);
    fake.chunk = 1;
    type(in);
    CHECK_EQ(n_executed, bulk_n);
    CHECK(!strcmp(fake.out, bulk));
}

// --- escape sequences (arrow keys etc.) -------------------------------------------------------

TEST(arrow_keys_are_ignored) {
    start(&io_plain);
    type("up\x1b[A\x1b[B\x1b[C\x1b[Dx\r");          // Up, Down, Right, Left
    CHECK_EQ(n_executed, 1);
    CHECK(!strcmp(executed[0], "upx"));
    CHECK_OUT("upx\n<upx>\nsos> ");                  // nothing echoed for the keys
}

TEST(csi_sequences_with_parameters_are_ignored) {
    start(&io_plain);
    type("a\x1b[3~b\x1b[1;5Cc\x1b[200~d\r");        // Delete, Ctrl+Right, paste start
    CHECK(!strcmp(executed[0], "abcd"));
}

TEST(ss3_sequences_are_ignored) {
    start(&io_plain);
    type("a\x1bOAb\x1bOPc\r");                      // application-mode Up, F1
    CHECK(!strcmp(executed[0], "abc"));
}

TEST(alt_key_is_ignored) {
    start(&io_plain);
    type("a\x1bxb\r");                                // Alt+x
    CHECK(!strcmp(executed[0], "ab"));
}

TEST(escape_sequence_split_across_reads) {
    start(&io_plain);
    type("a\x1b");
    type("[");
    type("1;5");
    type("Cb\r");
    CHECK_EQ(n_executed, 1);
    CHECK(!strcmp(executed[0], "ab"));
}

TEST(control_character_cancels_escape) {
    start(&io_plain);
    type("ab\x1b\r");                                 // stray ESC doesn't swallow Enter
    CHECK_EQ(n_executed, 1);
    CHECK(!strcmp(executed[0], "ab"));
    type("cd\x1b[\x03");                              // nor Ctrl-C inside a sequence
    CHECK_EQ(n_executed, 1);
    static const char tail[] = "cd^C\nsos> ";
    CHECK(!strcmp(fake.out + strlen(fake.out) - (sizeof(tail) - 1), tail));
    type("ef\x1b[1\r");                               // nor Enter inside a sequence
    CHECK_EQ(n_executed, 2);
    CHECK(!strcmp(executed[1], "ef"));
}

TEST(state_is_reset_by_init) {
    start(&io_plain);
    type("\x1b[");                                     // session ends mid-sequence
    start(&io_plain);
    type("Ab\r");                                      // new session: plain text
    CHECK(!strcmp(executed[0], "Ab"));
}

// --- questions (shell_ask) and input redraw ---------------------------------------------------

static char answers[4][SHELL_LINE_MAX + 1];
static int n_answers, n_cancelled;

static void on_answer(shell_t *s, char *answer, void *arg) {
    (void)arg;
    if (!answer) {
        n_cancelled++;
        shell_printf(s, "cancelled\n");
        return;
    }
    strcpy(answers[n_answers++ % 4], answer);
}

static void on_first(shell_t *s, char *answer, void *arg) {   // chains a second, secret question
    on_answer(s, answer, arg);
    if (answer)
        shell_ask(s, "Password: ", true, on_answer, NULL);
}

static void start_ask(const shell_io_t *io) {
    start(io);
    n_answers = n_cancelled = 0;
}

TEST(answer_goes_to_callback_not_command) {
    start_ask(&io_plain);
    shell_ask(&sh, "Name: ", false, on_answer, NULL);
    shell_show_input(&sh);                      // the question's prompt
    type("  bob  \r");
    CHECK_EQ(n_executed, 0);
    CHECK_EQ(n_answers, 1);
    CHECK(!strcmp(answers[0], "  bob  "));     // answers are not trimmed
    CHECK_OUT("Name:   bob  \nsos> ");          // back to the normal prompt
    type("x\r");
    CHECK_EQ(n_executed, 1);                    // next line is a command again
}

TEST(secret_answer_is_not_echoed) {
    start_ask(&io_plain);
    shell_ask(&sh, "Password: ", true, on_answer, NULL);
    type("hunter2x\x7f\r");                    // backspace works, silently
    CHECK(!strcmp(answers[0], "hunter2"));
    CHECK(strstr(fake.out, "hunter") == NULL);
    CHECK_OUT("\nsos> ");
}

TEST(empty_answer_is_passed) {
    start_ask(&io_plain);
    shell_ask(&sh, "Q: ", false, on_answer, NULL);
    type("\r");
    CHECK_EQ(n_answers, 1);
    CHECK_EQ(answers[0][0], 0);
}

TEST(ctrl_c_cancels_question) {
    start_ask(&io_plain);
    shell_ask(&sh, "Q: ", true, on_answer, NULL);
    type("abc\x03");
    CHECK_EQ(n_cancelled, 1);
    CHECK_EQ(n_answers, 0);
    CHECK_OUT("^C\ncancelled\nsos> ");
    type("d\r");                               // echo is back on, and it's a command again
    CHECK(!strcmp(executed[0], "d"));
}

// Ctrl-D: with shell_ask_eof it answers SHELL_EOF and nothing is logged out; with a plain
// question it still logs out (and the question stays pending).
static int n_eof;
static void on_eof_answer(shell_t *s, char *answer, void *arg) {
    (void)arg;
    if (answer == SHELL_EOF) {
        n_eof++;
        return;
    }
    on_answer(s, answer, arg);
    shell_ask_eof(s, "> ", on_eof_answer, NULL);      // keep asking, like an interactive prompt
}

TEST(ctrl_d_ends_input_for_eof_question) {
    start_ask(&io_plain);
    n_eof = 0;
    shell_ask_eof(&sh, "> ", on_eof_answer, NULL);
    type("1+1\r");
    CHECK_EQ(n_answers, 1);
    CHECK(!strcmp(answers[0], "1+1"));
    CHECK_OUT("1+1\n> ");
    clear_out();
    type("x\x04");                              // not on an empty line: ignored
    CHECK_EQ(n_eof, 0);
    type("\b\x04");                            // empty line now: end of input
    CHECK_EQ(n_eof, 1);
    CHECK_EQ(fake.logouts, 0);
    type("ls\r");                               // the question is gone: a command again
    CHECK(!strcmp(executed[0], "ls"));
}

TEST(cancelled_question_gives_commands_back) {
    start_ask(&io_plain);
    shell_ask_eof(&sh, "> ", on_answer, NULL);
    shell_cancel_question(&sh);
    type("ls\r");                               // a command again, not an answer
    CHECK_EQ(n_answers, 0);
    CHECK(!strcmp(executed[0], "ls"));
    type("\x04");                               // and Ctrl-D logs out again
    CHECK_EQ(fake.logouts, 1);
}

TEST(ctrl_d_on_plain_question_still_logs_out) {
    start_ask(&io_plain);
    shell_ask(&sh, "Q: ", false, on_answer, NULL);
    type("\x04");
    CHECK_EQ(fake.logouts, 1);
    CHECK_EQ(n_answers, 0);
    type("a\r");                                // still waiting for the answer
    CHECK_EQ(n_answers, 1);
}

TEST(questions_chain) {
    start_ask(&io_plain);
    shell_ask(&sh, "User: ", false, on_first, NULL);
    type("alice\r");
    CHECK_OUT("alice\nPassword: ");             // second question's prompt follows
    type("s3cret\r");
    CHECK_EQ(n_answers, 2);
    CHECK(!strcmp(answers[0], "alice") && !strcmp(answers[1], "s3cret"));
    CHECK(strstr(fake.out, "s3cret") == NULL);
}

TEST(init_clears_pending_question) {
    start_ask(&io_plain);
    shell_ask(&sh, "Q: ", true, on_answer, NULL);
    start_ask(&io_plain);                       // new session on the same shell
    type("cmd\r");
    CHECK_EQ(n_answers, 0);
    CHECK_EQ(n_executed, 1);
    CHECK(strstr(fake.out, "cmd") != NULL);     // echoed: not secret anymore
}

TEST(hide_and_redraw_input) {
    start(&io_plain);
    type("hel");
    fake.out_len = 0;
    shell_hide_input(&sh);
    CHECK_OUT("\r\x1b[K");
    fake.out_len = 0;
    shell_show_input(&sh);
    CHECK_OUT("sos> hel");
    type("p\r");                               // the typed text survived the redraw
    CHECK(!strcmp(executed[0], "help"));
}

TEST(redraw_hides_secret_text) {
    start_ask(&io_plain);
    shell_ask(&sh, "Password: ", true, on_answer, NULL);
    type("abc");
    fake.out_len = 0;
    shell_show_input(&sh);
    CHECK_OUT("Password: ");
}

TEST(crlf_channel_translates_newlines) {
    start(&io_crlf);
    type("hi\r");
    CHECK_OUT("hi\r\n<hi>\r\nsos> ");
    fake.out_len = 0;
    shell_printf(&sh, "a\nb\n\nc");
    CHECK_OUT("a\r\nb\r\n\r\nc");
}

TEST(plain_channel_leaves_newlines) {
    start(&io_plain);
    shell_printf(&sh, "a\nb\n");
    CHECK_OUT("a\nb\n");
}

TEST(printf_truncates_long_output) {
    char long_text[400];
    memset(long_text, 'y', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';
    start(&io_plain);
    shell_printf(&sh, "%s", long_text);
    CHECK_EQ(fake.out_len, 255);                            // 256-byte buffer, NUL excluded
}

TEST(job_runs_in_steps_then_prompts) {
    start(&io_plain);
    job_steps = 3;
    job_partial = false;
    type("job\r");
    CHECK(shell_job_running(&sh));
    CHECK_OUT("job\n");                        // no prompt while the job runs
    CHECK_EQ(job_calls, 0);                     // the first step runs on the next poll
    clear_out();
    type("");
    CHECK_OUT("step 1\n");
    type("");
    type("");
    CHECK_EQ(job_calls, 3);
    CHECK(!shell_job_running(&sh));
    CHECK_OUT("step 1\nstep 2\nstep 3\nsos> ");
    type("");                                   // finished: not called again
    CHECK_EQ(job_calls, 3);
}

TEST(job_ends_output_with_newline_before_prompt) {
    start(&io_plain);
    job_steps = 1;
    job_partial = true;
    type("job\r");
    clear_out();
    type("");
    CHECK_OUT("step 1\nsos> ");
}

TEST(input_during_job_runs_afterwards) {
    start(&io_plain);
    job_steps = 3;
    job_partial = false;
    type("job\r");
    type("ls\r");                              // kept, not run during the job
    CHECK_EQ(n_executed, 1);
    CHECK_EQ(job_calls, 1);
    type("");
    type("");                                   // job finishes: prompt
    CHECK(!shell_job_running(&sh));
    CHECK_EQ(n_executed, 1);
    clear_out();
    type("");                                   // then the kept line runs, echoed as typed
    CHECK_EQ(n_executed, 2);
    CHECK(!strcmp(executed[1], "ls"));
    CHECK_OUT("ls\n<ls>\nsos> ");
}

TEST(ctrl_c_cancels_job) {
    start(&io_plain);
    job_steps = 100;
    job_partial = true;
    type("job\r");
    type("");
    clear_out();
    type("x\x03");                             // also drops "x", typed during the job
    CHECK_EQ(job_cancels, 1);
    CHECK(!shell_job_running(&sh));
    CHECK_OUT("\n^C\nsos> ");                 // the partial line is ended first
    type("a\r");                               // back to normal
    CHECK(!strcmp(executed[1], "a"));
}

TEST(piped_commands_run_in_order) {
    start(&io_plain);
    job_steps = 2;
    job_partial = false;
    feed("job\rls\rjob\rpwd\r");            // one read, like commands piped into ssh
    CHECK_EQ(n_executed, 1);                    // "ls" waits for the job
    for (int i = 0; i < 8; i++)
        feed("");
    CHECK(!shell_job_running(&sh));
    CHECK_EQ(n_executed, 4);
    CHECK(!strcmp(executed[1], "ls") && !strcmp(executed[2], "job") && !strcmp(executed[3], "pwd"));
}

TEST(busy_while_job_or_type_ahead) {
    start(&io_plain);
    CHECK(!shell_busy(&sh));
    job_steps = 2;
    job_partial = false;
    feed("job\rls\r");
    CHECK(shell_busy(&sh));                     // job running, "ls" waiting
    feed("");
    feed("");                                   // job done; "ls" still to run
    CHECK(!shell_job_running(&sh));
    CHECK(shell_busy(&sh));
    feed("");                                   // "ls" runs
    CHECK(!shell_busy(&sh));
}

TEST(full_type_ahead_leaves_input_unread) {
    static char lots[SHELL_LINE_MAX + 100];
    start(&io_plain);
    job_steps = 3;
    type("job\r");
    memset(lots, 'z', sizeof(lots) - 1);
    lots[sizeof(lots) - 1] = '\0';
    feed(lots);
    CHECK_EQ(fake.in_pos, SHELL_LINE_MAX);      // the rest stays in the channel
    CHECK_EQ(sh.ahead_len, SHELL_LINE_MAX);
}

TEST(shell_end_cancels_job) {
    start(&io_plain);
    job_steps = 100;
    type("job\r");
    type("");
    shell_end(&sh);
    CHECK_EQ(job_cancels, 1);
    CHECK(!shell_job_running(&sh));
    shell_end(&sh);                             // idempotent
    CHECK_EQ(job_cancels, 1);
}

TEST(init_resets_job_and_cwd) {
    start(&io_plain);
    strcpy(sh.cwd, "/docs");
    job_steps = 100;
    type("job\r");
    shell_end(&sh);
    shell_init(&sh, &io_plain, &fake);
    CHECK(!shell_job_running(&sh));
    CHECK(!strcmp(sh.cwd, "/"));
}

TEST(redraw_during_job_keeps_output) {
    start(&io_plain);
    job_steps = 100;
    job_partial = true;
    type("job\r");
    type("");                                   // "step 1" with no newline
    clear_out();
    shell_hide_input(&sh);                      // a log line arrives: end the line, don't erase
    CHECK_OUT("\n");
    clear_out();
    shell_show_input(&sh);                      // and no prompt afterwards
    CHECK_OUT("");
    job_partial = false;
    type("");                                   // "step 2\n"
    clear_out();
    shell_hide_input(&sh);                      // at a line start: nothing to do
    CHECK_OUT("");
}

TEST(write_space) {
    start(&io_plain);
    CHECK_EQ(shell_write_space(&sh), 1024);     // channel without a limit
    start(&io_space);
    fake_space = 300;
    CHECK_EQ(shell_write_space(&sh), 300);
    fake_space = -5;
    CHECK_EQ(shell_write_space(&sh), 0);
}

TEST(raw_write_translates_newlines) {
    start(&io_crlf);
    shell_write(&sh, "a\0b\n", 4);
    CHECK_EQ(fake.out_len, 5);
    CHECK(!memcmp(fake.out, "a\0b\r\n", 5));
}


// --- raw input mode (shell_run_raw) ----------------------------------------------------------

TEST(raw_job_gets_every_key) {
    start(&io_plain);
    type("raw\r");
    CHECK(shell_job_running(&sh));
    clear_out();
    type("a\x03\x04\x7f\x1b[A\x1b[3~\xc3\xa9");      // Ctrl-C and Ctrl-D are keys too
    CHECK(shell_job_running(&sh));
    CHECK_EQ(n_raw_keys, 8);
    CHECK_EQ(raw_keys_got[0], 'a');
    CHECK_EQ(raw_keys_got[1], 0x03);
    CHECK_EQ(raw_keys_got[2], 0x04);
    CHECK_EQ(raw_keys_got[3], 0x7f);
    CHECK_EQ(raw_keys_got[4], KEY_UP);
    CHECK_EQ(raw_keys_got[5], KEY_DELETE);
    CHECK_EQ(raw_keys_got[6], 0xc3);                    // UTF-8 bytes as they come
    CHECK_EQ(raw_keys_got[7], 0xa9);
    CHECK_OUT("");                                      // nothing echoed
    CHECK_EQ(raw_cancels, 0);
    CHECK_EQ(fake.logouts, 0);
    CHECK_EQ(n_executed, 1);
}

TEST(raw_enter_is_cr) {
    start(&io_plain);
    type("raw\r");
    type("\r\n\n\r");                                   // CRLF, LF, CR
    CHECK_EQ(n_raw_keys, 3);
    CHECK_EQ(raw_keys_got[0], '\r');
    CHECK_EQ(raw_keys_got[1], '\r');
    CHECK_EQ(raw_keys_got[2], '\r');
    type("\n");                                         // LF of a CRLF split across reads
    CHECK_EQ(n_raw_keys, 3);
}

TEST(raw_job_ends_with_its_step) {
    start(&io_plain);
    type("raw\r");
    clear_out();
    type("x\x11");                                      // Ctrl-Q: the step after it finishes
    CHECK(!shell_job_running(&sh));
    CHECK_EQ(raw_cancels, 0);
    CHECK_OUT("sos> ");
    type("ls\r");                                       // a command again
    CHECK_EQ(n_executed, 2);
    CHECK(!strcmp(executed[1], "ls"));
}

TEST(raw_job_gets_rest_of_command_read) {
    start(&io_plain);
    type("raw\rab\x1b[");                               // typed with the command
    CHECK_EQ(n_raw_keys, 0);
    type("B");                                           // sequence completed in the next read
    CHECK_EQ(n_raw_keys, 3);
    CHECK_EQ(raw_keys_got[0], 'a');
    CHECK_EQ(raw_keys_got[1], 'b');
    CHECK_EQ(raw_keys_got[2], KEY_DOWN);
}

TEST(raw_input_is_bounded_per_pass) {
    static char lots[600];
    memset(lots, 'z', sizeof(lots));
    start(&io_plain);
    type("raw\r");
    fake.in = lots;
    fake.in_len = sizeof(lots);
    fake.in_pos = 0;
    shell_poll(&sh);
    CHECK(fake.in_pos >= 256 && fake.in_pos < sizeof(lots));
    CHECK_EQ(raw_steps, 1);                             // a step after each bounded batch
    while (fake.in_pos < fake.in_len)
        shell_poll(&sh);
    CHECK_EQ(n_raw_keys, (int)sizeof(lots));
}

TEST(shell_end_cancels_raw_job) {
    start(&io_plain);
    type("raw\r");
    type("q");
    shell_end(&sh);
    CHECK(!shell_job_running(&sh));
    CHECK_EQ(raw_cancels, 1);
    start(&io_plain);                                   // next session: lines again
    type("ab\r");
    CHECK_EQ(n_executed, 1);
    CHECK(!strcmp(executed[0], "ab"));
}

TEST(raw_mode_and_redraw) {
    start(&io_plain);
    CHECK(!shell_raw_mode(&sh));
    job_steps = 2;
    job_partial = false;
    type("job\r");
    CHECK(!shell_raw_mode(&sh));                        // a plain job isn't raw
    type("");
    type("");
    type("raw\r");
    CHECK(shell_raw_mode(&sh));
    clear_out();
    shell_hide_input(&sh);                              // log output around a raw job: nothing
    shell_show_input(&sh);
    CHECK_OUT("");
    type("\x11");
    CHECK(!shell_raw_mode(&sh));
}

TEST(plain_job_after_raw_job_still_stashes) {
    start(&io_plain);
    type("raw\r");
    type("\x11");
    CHECK(!shell_job_running(&sh));
    job_steps = 2;
    job_partial = false;
    type("job\r");
    type("ls\r");                                       // a plain job keeps input for later
    CHECK_EQ(n_raw_keys, 1);
    type("");                                           // step 2: the job ends
    type("");                                           // then the kept line runs
    CHECK_EQ(n_executed, 3);
    CHECK(!strcmp(executed[2], "ls"));
}

// UTF-8 is taken into the line (a Wi-Fi network named "Mjölnir"); Backspace removes a whole
// character, and as many columns as it takes.
TEST(utf8_characters_are_taken) {
    start(&io_plain);
    type("wifi Mj\xc3\xb6lnir\r");
    CHECK_EQ(n_executed, 1);
    CHECK(!strcmp(executed[0], "wifi Mj\xc3\xb6lnir"));
    CHECK_OUT("wifi Mj\xc3\xb6lnir\n<wifi Mj\xc3\xb6lnir>\nsos> ");
}

TEST(utf8_byte_at_a_time) {
    fake.chunk = 0;
    start(&io_plain);
    fake.chunk = 1;
    type("\xe2\x82\xac" "5\r");                                // euro sign, three bytes
    CHECK(!strcmp(executed[0], "\xe2\x82\xac" "5"));
}

TEST(backspace_removes_a_whole_utf8_character) {
    start(&io_plain);
    type("a\xc3\xb6\x7f" "b\r");                             // ö then Backspace
    CHECK(!strcmp(executed[0], "ab"));
    CHECK_OUT("a\xc3\xb6\b \bb\n<ab>\nsos> ");
    start(&io_plain);
    type("\xe4\xb8\xad\x7f\r");                              // a wide CJK character: 2 columns
    CHECK_EQ(n_executed, 0);
    CHECK_OUT("\xe4\xb8\xad\b \b\b \b\nsos> ");
}

TEST(broken_utf8_is_dropped) {
    start(&io_plain);
    type("a\xb6" "b\xc3" "c\xc0\xaf" "d\xf8" "e\xc3\r");     // stray, cut short, overlong, invalid
    CHECK(!strcmp(executed[0], "abcde"));
    start(&io_plain);
    type("x\xc3\xff\xc3\xc1\xc3\xf5\r");                 // invalid bytes can't continue one
    CHECK(!strcmp(executed[0], "x"));
    start(&io_plain);
    type("y\xc3\xff\xb6\r");                              // ...nor end it: the ö still completes
    CHECK(!strcmp(executed[0], "y\xc3\xb6"));
}

TEST(utf8_character_must_fit_the_line) {
    char in[SHELL_LINE_MAX + 4];
    memset(in, 'x', SHELL_LINE_MAX - 1);
    memcpy(in + SHELL_LINE_MAX - 1, "\xc3\xb6\r", 3);         // one byte of room: ö doesn't fit
    start(&io_plain);
    type_n(in, SHELL_LINE_MAX + 2);
    CHECK_EQ(n_executed, 1);
    CHECK_EQ(strlen(executed[0]), SHELL_LINE_MAX - 1);
}

// --- command history (Up/Down) ---------------------------------------------------------------

#define UP "\x1b[A"
#define DOWN "\x1b[B"

// Does the history hold this text anywhere?
static bool history_has(const char *text) {
    size_t n = strlen(text);
    for (size_t i = 0; i + n <= sizeof(sh.history); i++)
        if (!memcmp(sh.history + i, text, n))
            return true;
    return false;
}

// Is the line being typed exactly this?
static bool line_is(const char *text) {
    return sh.line_len == strlen(text) && !memcmp(sh.line, text, sh.line_len);
}

TEST(up_recalls_previous_commands) {
    start(&io_plain);
    type("one\rtwo\r");
    clear_out();
    type(UP);
    CHECK_OUT("\r\x1b[Ksos> two");                 // the line is cleared and redrawn
    CHECK(line_is("two"));
    type(UP);
    CHECK(line_is("one"));
    type("\r");
    CHECK_EQ(n_executed, 3);
    CHECK(!strcmp(executed[2], "one"));
}

TEST(up_stops_at_the_oldest) {
    start(&io_plain);
    type("one\rtwo\r" UP UP);
    clear_out();
    type(UP);                                        // nothing older: nothing happens
    CHECK_OUT("");
    CHECK(line_is("one"));
}

TEST(down_goes_forward_then_back_to_the_draft) {
    start(&io_plain);
    type("one\rtwo\rdra");
    type(UP UP);
    CHECK(line_is("one"));
    type(DOWN);
    CHECK(line_is("two"));
    clear_out();
    type(DOWN);                                      // past the newest: what was being typed
    CHECK_OUT("\r\x1b[Ksos> dra");
    CHECK(line_is("dra"));
    clear_out();
    type(DOWN);                                      // and no further
    CHECK_OUT("");
    type("ft\r");
    CHECK(!strcmp(executed[2], "draft"));
}

TEST(up_and_down_with_no_history_do_nothing) {
    start(&io_plain);
    type("ab" UP DOWN "c\r");
    CHECK(!strcmp(executed[0], "abc"));
    CHECK_OUT("abc\n<abc>\nsos> ");
}

TEST(history_keeps_commands_trimmed_and_skips_repeats) {
    start(&io_plain);
    type("  ls  \rls\r\r   \rpwd\rpwd\r");
    type(UP);
    CHECK(line_is("pwd"));
    type(UP);
    CHECK(line_is("ls"));                            // one "ls", one "pwd"; no empty lines
    clear_out();
    type(UP);
    CHECK_OUT("");
}

TEST(an_edited_recalled_line_is_a_new_command) {
    start(&io_plain);
    type("one\rtwo\r" UP "\x7f" "o\r");             // "two" recalled, edited to "two" -> "tw" + "o"
    CHECK(!strcmp(executed[2], "two"));
    type(UP "x\r");
    CHECK(!strcmp(executed[3], "twox"));
    type(UP);
    CHECK(line_is("twox"));
    type(UP);
    CHECK(line_is("two"));                           // the repeat of "two" wasn't stored twice
    type(UP);
    CHECK(line_is("one"));
}

TEST(ctrl_c_after_up_starts_again_from_the_newest) {
    start(&io_plain);
    type("one\rtwo\r" UP UP "\x03");
    CHECK_EQ(sh.line_len, 0);
    type(UP);
    CHECK(line_is("two"));
}

TEST(answers_are_not_in_the_history) {
    start_ask(&io_plain);
    type("passwd\r");
    shell_ask(&sh, "Password: ", true, on_answer, NULL);
    type("hunter22\r");
    shell_ask(&sh, "Name: ", false, on_answer, NULL);
    clear_out();
    type(UP);                                        // during a question: nothing
    CHECK_OUT("");
    CHECK_EQ(sh.line_len, 0);
    type("bob\r");
    CHECK_EQ(n_answers, 2);
    type(UP);
    CHECK(line_is("passwd"));                        // only the command
    clear_out();
    type(UP);
    CHECK_OUT("");
    CHECK(!history_has("hunter22"));
    CHECK(!history_has("bob"));
}

TEST(history_is_per_session_and_wiped) {
    start(&io_plain);
    type("secret-cmd\r");
    start(&io_plain);                                // a new session (shell_init)
    clear_out();
    type(UP);
    CHECK_OUT("");
    type("again\r");
    shell_discard_input(&sh);                        // the console locking itself
    type(UP);
    CHECK_EQ(sh.line_len, 0);
    CHECK_EQ(sh.history_len, 0);
    type("third\r");
    shell_clear_history(&sh);                        // `logout` on the console
    type(UP);
    CHECK_EQ(sh.line_len, 0);
    for (size_t i = 0; i < sizeof(sh.history); i++)
        CHECK(sh.history[i] == 0);
}

TEST(oldest_commands_make_room) {
    start(&io_plain);
    char cmd[120];
    for (int i = 0; i < 20; i++) {                   // each 101 bytes stored: 10 fit in 1024
        snprintf(cmd, sizeof(cmd), "c%02d%097d\r", i, 0);
        type_n(cmd, strlen(cmd));
    }
    CHECK(sh.history_len <= sizeof(sh.history));
    int seen = 0;
    char newest[4] = "", oldest[4] = "";
    for (;;) {
        clear_out();
        type(UP);
        if (!fake.out_len)
            break;
        if (!seen)
            memcpy(newest, sh.line, 3);
        memcpy(oldest, sh.line, 3);
        seen++;
    }
    CHECK_EQ(seen, 10);
    CHECK(!memcmp(newest, "c19", 3));
    CHECK(!memcmp(oldest, "c10", 3));
}

TEST(history_on_a_crlf_channel) {
    start(&io_crlf);
    type("one\r");
    clear_out();
    type(UP);
    CHECK_OUT("\r\x1b[Ksos> one");
}

TEST_MAIN(
    RUN(utf8_characters_are_taken);
    RUN(utf8_byte_at_a_time);
    RUN(backspace_removes_a_whole_utf8_character);
    RUN(broken_utf8_is_dropped);
    RUN(utf8_character_must_fit_the_line);
    RUN(init_prints_greeting_and_prompt);
    RUN(command_is_echoed_and_executed);
    RUN(line_endings);
    RUN(crlf_split_across_reads_counts_once);
    RUN(empty_lines_prompt_without_executing);
    RUN(lf_then_cr_is_two_lines);
    RUN(spaces_are_trimmed);
    RUN(backspace_and_delete_edit_the_line);
    RUN(backspace_on_empty_line_does_nothing);
    RUN(ctrl_c_discards_the_line);
    RUN(ctrl_d_on_empty_line_logs_out);
    RUN(ctrl_d_with_text_is_ignored);
    RUN(ctrl_d_on_console_explains);
    RUN(logout_uses_channel);
    RUN(line_length_is_limited);
    RUN(control_and_high_bytes_are_ignored);
    RUN(byte_at_a_time_matches_bulk);
    RUN(arrow_keys_are_ignored);
    RUN(csi_sequences_with_parameters_are_ignored);
    RUN(ss3_sequences_are_ignored);
    RUN(alt_key_is_ignored);
    RUN(escape_sequence_split_across_reads);
    RUN(control_character_cancels_escape);
    RUN(state_is_reset_by_init);
    RUN(answer_goes_to_callback_not_command);
    RUN(secret_answer_is_not_echoed);
    RUN(empty_answer_is_passed);
    RUN(ctrl_c_cancels_question);
    RUN(ctrl_d_ends_input_for_eof_question);
    RUN(ctrl_d_on_plain_question_still_logs_out);
    RUN(cancelled_question_gives_commands_back);
    RUN(questions_chain);
    RUN(init_clears_pending_question);
    RUN(hide_and_redraw_input);
    RUN(redraw_hides_secret_text);
    RUN(crlf_channel_translates_newlines);
    RUN(plain_channel_leaves_newlines);
    RUN(printf_truncates_long_output);
    RUN(job_runs_in_steps_then_prompts);
    RUN(job_ends_output_with_newline_before_prompt);
    RUN(input_during_job_runs_afterwards);
    RUN(ctrl_c_cancels_job);
    RUN(piped_commands_run_in_order);
    RUN(busy_while_job_or_type_ahead);
    RUN(full_type_ahead_leaves_input_unread);
    RUN(shell_end_cancels_job);
    RUN(init_resets_job_and_cwd);
    RUN(redraw_during_job_keeps_output);
    RUN(write_space);
    RUN(raw_write_translates_newlines);
    RUN(raw_job_gets_every_key);
    RUN(raw_enter_is_cr);
    RUN(raw_job_ends_with_its_step);
    RUN(raw_job_gets_rest_of_command_read);
    RUN(raw_input_is_bounded_per_pass);
    RUN(shell_end_cancels_raw_job);
    RUN(raw_mode_and_redraw);
    RUN(plain_job_after_raw_job_still_stashes);
    RUN(up_recalls_previous_commands);
    RUN(up_stops_at_the_oldest);
    RUN(down_goes_forward_then_back_to_the_draft);
    RUN(up_and_down_with_no_history_do_nothing);
    RUN(history_keeps_commands_trimmed_and_skips_repeats);
    RUN(an_edited_recalled_line_is_a_new_command);
    RUN(ctrl_c_after_up_starts_again_from_the_newest);
    RUN(answers_are_not_in_the_history);
    RUN(history_is_per_session_and_wiped);
    RUN(oldest_commands_make_room);
    RUN(history_on_a_crlf_channel);
)
