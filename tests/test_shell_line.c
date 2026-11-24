// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/shell_core.c: shell line editing and output.

#include <stdint.h>

#include "shell.h"
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

void shell_execute(shell_t *sh, char *cmd) {
    if (n_executed < 16)
        strcpy(executed[n_executed], cmd);
    n_executed++;
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
    CHECK_OUT("\nType 'help' for commands.\nsos> ");
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

TEST_MAIN(
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
    RUN(crlf_channel_translates_newlines);
    RUN(plain_channel_leaves_newlines);
    RUN(printf_truncates_long_output);
)
