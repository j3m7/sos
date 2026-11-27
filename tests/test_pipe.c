// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/shell/pipe.c: the stage parser and the filters grep, head, tail and wc.

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

#include "core/heaptag.h"
#include "shell/pipe.h"
#include "shell/shell.h"
#include "test.h"

// --- fakes -----------------------------------------------------------------------------------

static shell_t sh;
static char said[512];          // what the stages printed (usage, errors)

void shell_printf(shell_t *s, const char *fmt, ...) {
    (void)s;
    size_t n = strlen(said);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(said + n, sizeof(said) - n, fmt, ap);
    va_end(ap);
}

static char out[16384];         // what reached the sink
static size_t out_len;

static void sink(shell_t *s, const char *buf, size_t len) {
    CHECK(s == &sh);
    CHECK(len > 0);
    CHECK(out_len + len < sizeof(out));
    if (out_len + len >= sizeof(out))
        return;
    memcpy(out + out_len, buf, len);
    out_len += len;
    out[out_len] = '\0';
}

// --- helpers ---------------------------------------------------------------------------------

static pipeline_t p;

static bool open_stages(const char *stages) {
    static char text[256];
    snprintf(text, sizeof(text), "%s", stages);
    said[0] = '\0';
    out_len = 0;
    out[0] = '\0';
    memset(&p, 0, sizeof(p));
    if (!pipe_open(&sh, &p, text))
        return false;
    p.out = sink;
    return true;
}

// Run `stages` over in[0..len): in two writes split at `cut` (0 or len: one write), or one byte
// a write when `cut` is SIZE_MAX. Then end the chain. The output is in `out`; *done says the
// stream was shut.
static void run_cut(const char *stages, const char *in, size_t len, size_t cut, bool *done) {
    CHECK(open_stages(stages));
    if (cut == SIZE_MAX) {
        for (size_t i = 0; i < len; i++)
            pipe_write(&p, in + i, 1);
    } else {
        pipe_write(&p, in, cut);
        pipe_write(&p, in + cut, len - cut);
    }
    if (done)
        *done = p.done;
    pipe_end(&p);
    CHECK(p.first == NULL && !p.done);
}

// The output of `stages` over `in`, checked to be the same however the input is split: at every
// byte boundary, and a byte at a time.
static const char *run_n(const char *stages, const char *in, size_t len) {
    static char whole[sizeof(out)];
    size_t used = heap_tag_used(HEAP_EDIT);
    run_cut(stages, in, len, 0, NULL);
    memcpy(whole, out, out_len + 1);
    size_t whole_len = out_len;
    for (size_t cut = 1; cut <= len; cut++) {
        run_cut(stages, in, len, cut, NULL);
        if (out_len != whole_len || memcmp(out, whole, whole_len)) {
            fprintf(stderr, "  %s: split at %zu: got \"%s\", whole \"%s\"\n", stages, cut, out,
                    whole);
            CHECK(false);
            break;
        }
    }
    run_cut(stages, in, len, SIZE_MAX, NULL);
    CHECK(out_len == whole_len && !memcmp(out, whole, whole_len));
    CHECK_EQ(heap_tag_used(HEAP_EDIT), used);       // everything freed
    memcpy(out, whole, whole_len + 1);
    out_len = whole_len;
    return out;
}
#define run(stages, in) run_n((stages), (in), strlen(in))

#define CHECK_STR(got, want) do {                                                           \
        const char *g_ = (got), *w_ = (want);                                               \
        test_checks++;                                                                      \
        if (strcmp(g_, w_)) {                                                               \
            test_failures++;                                                                \
            fprintf(stderr, "%s:%d: %s:\n  got:      \"%s\"\n  expected: \"%s\"\n",          \
                    __FILE__, __LINE__, test_current, g_, w_);                              \
        }                                                                                   \
    } while (0)

// --- tests -----------------------------------------------------------------------------------

static const char *const text = "a fox\nno\nFox two\n\nthe last fox";

TEST(grep_passes_matching_lines) {
    CHECK_STR(run("grep fox", text), "a fox\nthe last fox\n");      // a newline added at the end
    CHECK_STR(run("grep -i fox", text), "a fox\nFox two\nthe last fox\n");
    CHECK_STR(run("grep -v fox", text), "no\nFox two\n\n");
    CHECK_STR(run("grep -n fox", text), "1:a fox\n5:the last fox\n");
    CHECK_STR(run("grep -vn -i fox", text), "2:no\n4:\n");
    CHECK_STR(run("grep -v -n -i fox", text), "2:no\n4:\n");
    CHECK_STR(run("grep zebra", text), "");
    CHECK_STR(run("grep FOX", text), "");                           // exact case without -i
    CHECK_STR(run("grep -i fOX", "x\nFOX\n"), "FOX\n");
}

TEST(grep_text_keeps_its_spaces_and_may_start_with_a_dash) {
    CHECK_STR(run("grep  last fox ", text), "the last fox\n");
    CHECK_STR(run("grep a  fox", text), "");                        // two spaces: not "a fox"
    CHECK_STR(run("grep -x", "a-x\nb\n"), "a-x\n");                 // not an option: the text
    CHECK_STR(run("grep -v -", "a-x\nb\n"), "b\n");
    CHECK_STR(run("grep a|b", "a|b\nab\n"), "a|b\n");               // a '|' before no stage name
}

TEST(grep_cuts_long_lines_into_pieces) {
    char in[1400];
    memset(in, 'a', 1200);
    memcpy(in + 600, "fox", 3);             // in the second piece (bytes 512-1023)
    in[1200] = '\n';
    strcpy(in + 1201, "fox\n");
    const char *got = run("grep fox", in);
    CHECK_EQ(strlen(got), 512 + 1 + 4);
    CHECK(!memcmp(got + 600 - 512, "fox", 3));
    CHECK(!strcmp(got + 513, "fox\n"));
    // Pieces of one line share its number.
    got = run("grep -n a", in);
    CHECK(!strncmp(got, "1:aaa", 5));
    CHECK_EQ(strlen(got), 3 * 2 + 512 + 512 + 176 + 3);
    // A line of exactly PIPE_LINE_MAX bytes is one piece.
    memset(in, 'b', PIPE_LINE_MAX);
    strcpy(in + PIPE_LINE_MAX, "\nb\n");
    got = run("grep -n b", in);
    CHECK_EQ(strlen(got), 2 + PIPE_LINE_MAX + 1 + 4);
    CHECK(!strcmp(got + 2 + PIPE_LINE_MAX + 1, "2:b\n"));
    // ...and its '\n' doesn't make an empty second piece: -v shows no blank line for it.
    got = run("grep -vn fox", in);
    CHECK_EQ(strlen(got), 2 + PIPE_LINE_MAX + 1 + 4);
    CHECK(!strcmp(got + 2 + PIPE_LINE_MAX + 1, "2:b\n"));
    memset(in, 'c', 2 * PIPE_LINE_MAX);
    strcpy(in + 2 * PIPE_LINE_MAX, "\n");
    got = run("grep -v fox", in);
    CHECK_EQ(strlen(got), 2 * (PIPE_LINE_MAX + 1));
}

TEST(head_passes_the_first_lines_then_shuts) {
    CHECK_STR(run("head 2", "1\n2\n3\n4\n"), "1\n2\n");
    CHECK_STR(run("head", "1\n2\n3\n"), "1\n2\n3\n");
    CHECK_STR(run("head 3", "1\n2"), "1\n2");                       // as it came
    bool done;
    run_cut("head 2", "1\n2\n3\n", 6, 0, &done);
    CHECK(done);
    run_cut("head 2", "1\n2\n3\n", 6, 3, &done);
    CHECK(done);
    run_cut("head 4", "1\n2\n3\n", 6, 0, &done);
    CHECK(!done);
    // Once shut, what follows is dropped before any stage sees it.
    CHECK(open_stages("head 1 | wc"));
    pipe_write(&p, "a b\nc\n", 6);
    CHECK(p.done);
    pipe_write(&p, "more\n", 5);
    pipe_end(&p);
    CHECK_STR(out, "1 2 4\n");
}

TEST(tail_keeps_the_last_lines) {
    CHECK_STR(run("tail 2", "a\nb\nc\n"), "b\nc\n");
    CHECK_STR(run("tail 2", "a\nb\nc"), "b\nc");                    // a last line without '\n'
    CHECK_STR(run("tail 5", "a\nb\n"), "a\nb\n");
    CHECK_STR(run("tail", "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n"),
              "3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n");
    CHECK_STR(run("tail 1", "\n\n\n"), "\n");
    CHECK_STR(run("tail 3", ""), "");
}

TEST(tail_evicts_whole_lines_at_the_ring_cap) {
    // 100 lines of 100 bytes: the ring (4 KB) holds 40 of them whole, whatever was asked.
    static char in[10001];
    for (int i = 0; i < 100; i++)
        snprintf(in + i * 100, 101, "line %03d %090d\n", i, 0);
    const char *got = run_n("tail 50", in, 10000);
    CHECK_EQ(strlen(got), 40 * 100);
    CHECK(!strncmp(got, "line 060 ", 9));
    CHECK(!strcmp(got + 3900, in + 9900));
    // A line longer than the ring: its end.
    static char big[5003];
    memset(big, 'x', 5000);
    strcpy(big + 5000, "\n");
    got = run_n("tail 1", big, 5001);
    CHECK_EQ(strlen(got), TAIL_MAX);
    CHECK(got[TAIL_MAX - 1] == '\n' && got[0] == 'x' && got[TAIL_MAX - 2] == 'x');
}

TEST(wc_counts_as_unix_wc) {
    CHECK_STR(run("wc", "hello world\n  two  words \n\nlast"), "3 5 31\n");
    CHECK_STR(run("wc", ""), "0 0 0\n");
    CHECK_STR(run("wc", "one\ttwo\rthree\vfour\ffive\n"), "1 5 24\n");
    CHECK_STR(run("wc", "caf\xc3\xa9 ol\xc3\xa9\n"), "1 2 11\n");   // bytes, not characters
}

TEST(stages_chain) {
    CHECK_STR(run("grep o | head 2", "one\ntwo\nthree\nfour\n"), "one\ntwo\n");
    CHECK_STR(run("head 2 | wc", "one two\nthree\nfour\n"), "2 3 14\n");
    CHECK_STR(run("tail 3 | grep -n f", "a\nfive\nb\nfour\n"), "1:five\n3:four\n");
    CHECK_STR(run("grep -v x|tail 1|wc", "a\nx\nb c\n"), "1 2 4\n");
    CHECK(open_stages("grep x | more"));
    CHECK(p.sink == PIPE_SINK_PAGER && p.first != NULL);
    pipe_close(&p);
    CHECK(open_stages("more"));
    CHECK(p.sink == PIPE_SINK_PAGER && p.first == NULL);
    pipe_write(&p, "as is\n", 6);           // no filter: straight to the sink
    CHECK_STR(out, "as is\n");
    CHECK(open_stages("wc"));
    CHECK(p.sink == PIPE_SINK_TERMINAL);
    pipe_close(&p);
}

TEST(close_drops_what_is_held) {
    size_t used = heap_tag_used(HEAP_EDIT);
    CHECK(open_stages("grep a | tail 2 | wc"));
    CHECK(heap_tag_used(HEAP_EDIT) > used);
    pipe_write(&p, "a\nb\na\npartial a", 15);
    pipe_close(&p);
    CHECK_EQ(out_len, 0);
    CHECK(p.first == NULL);
    CHECK_EQ(heap_tag_used(HEAP_EDIT), used);
}

TEST(stages_that_do_not_parse_say_why) {
    static const struct {
        const char *stages, *says;
    } bad[] = {
        {"grep", "usage: <command> | grep [-v] [-i] [-n] <text>\n"},
        {"grep -v -n", "usage: <command> | grep"},
        {"grep 01234567890123456789012345678901234567890123456789012345678901234",
         "grep: the text is longer than 64 bytes\n"},
        {"head x", "usage: <command> | head [<lines>]"},
        {"head 0", "usage: <command> | head"},
        {"head 1000000", "usage: <command> | head"},
        {"tail -3", "usage: <command> | tail"},
        {"wc -l", "usage: <command> | wc\n"},
        {"more x", "usage: <command> | more\n"},
        {"more | wc", "more: only at the end of the line\n"},
        {"head | wc | tail | head | grep x", "grep: 4 filters in a line at most\n"},
        {"grep x | head y | more", "usage: <command> | head"},
    };
    size_t used = heap_tag_used(HEAP_EDIT);
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (open_stages(bad[i].stages)) {
            fprintf(stderr, "  \"%s\" parsed\n", bad[i].stages);
            CHECK(false);
            pipe_close(&p);
            continue;
        }
        if (strncmp(said, bad[i].says, strlen(bad[i].says)))
            fprintf(stderr, "  \"%s\" said \"%s\"\n", bad[i].stages, said);
        CHECK(!strncmp(said, bad[i].says, strlen(bad[i].says)));
        CHECK(p.first == NULL && p.sink == PIPE_SINK_TERMINAL);
    }
    CHECK_EQ(heap_tag_used(HEAP_EDIT), used);   // what parsed before the bad stage is freed
    CHECK(open_stages("head 999999 | tail 1 | grep -vin x | wc"));     // four is fine
    pipe_close(&p);
}

TEST(split_finds_the_stages) {
    char line[64];
    strcpy(line, "ls | morex");                 // no stage: as typed
    CHECK(pipe_split(line) == NULL);
    CHECK_STR(line, "ls | morex");
    strcpy(line, "lua -e print(1|2)");
    CHECK(pipe_split(line) == NULL);
    strcpy(line, "help|more");
    CHECK_STR(pipe_split(line), "more");
    CHECK_STR(line, "help");
    strcpy(line, "dmesg  |  grep a|b | head");
    CHECK_STR(pipe_split(line), "  grep a|b | head");
    CHECK_STR(line, "dmesg");
    strcpy(line, "a | b | wc");                 // the first '|' before a stage name
    CHECK_STR(pipe_split(line), " wc");
    CHECK_STR(line, "a | b");
    strcpy(line, "x |grepx | head|wc");
    CHECK_STR(pipe_split(line), " head|wc");
    CHECK_STR(line, "x |grepx");
    strcpy(line, "| grep x");
    CHECK_STR(pipe_split(line), " grep x");
    CHECK_STR(line, "");
    CHECK(pipe_is_stage("wc") && pipe_is_stage("tail 3") && pipe_is_stage("more|"));
    CHECK(!pipe_is_stage("wcx") && !pipe_is_stage("") && !pipe_is_stage("Grep"));
    // "head|wc": two stages.
    CHECK(open_stages(" head 1|wc"));
    pipe_write(&p, "a\nb\n", 4);
    pipe_end(&p);
    CHECK_STR(out, "1 1 2\n");
}

TEST_MAIN(
    RUN(grep_passes_matching_lines);
    RUN(grep_text_keeps_its_spaces_and_may_start_with_a_dash);
    RUN(grep_cuts_long_lines_into_pieces);
    RUN(head_passes_the_first_lines_then_shuts);
    RUN(tail_keeps_the_last_lines);
    RUN(tail_evicts_whole_lines_at_the_ring_cap);
    RUN(wc_counts_as_unix_wc);
    RUN(stages_chain);
    RUN(close_drops_what_is_held);
    RUN(stages_that_do_not_parse_say_why);
    RUN(split_finds_the_stages);
)
