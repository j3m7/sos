// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/shell/words.c: the shell's tokenizer and the join for flat-string handlers.

#include <stdbool.h>

#include "shell/words.h"
#include "test.h"

// The words of `line` as "[word]" each, operators as "<|>", "<>>", "<>>>"; or "!" and the error.
static const char *split(const char *line) {
    static char buf[300], out[1024];
    snprintf(buf, sizeof(buf), "%s", line);
    words_t w;
    const char *why = words_split(buf, &w);
    if (why) {
        snprintf(out, sizeof(out), "!%s", why);
        return out;
    }
    size_t n = 0;
    out[0] = '\0';
    CHECK(w.n >= 0 && w.n <= SHELL_WORDS_MAX && w.v[w.n] == NULL);
    for (int i = 0; i < w.n; i++) {
        const char *op = w.kind[i] == WORD_PIPE ? "|" : w.kind[i] == WORD_REDIRECT ? ">"
                         : w.kind[i] == WORD_APPEND ? ">>" : NULL;
        CHECK((op == NULL) == (w.v[i] != NULL));
        CHECK(!w.v[i] || (w.v[i] >= buf && w.v[i] + strlen(w.v[i]) < buf + strlen(line) + 1));
        n += (size_t)snprintf(out + n, sizeof(out) - n, op ? "<%s>" : "[%s]", op ? op : w.v[i]);
    }
    return out;
}

TEST(splits_every_rule) {
    static const struct {
        const char *line, *want;
    } cases[] = {
        // Spaces separate words; runs of them count once.
        {"", ""},
        {"   ", ""},
        {"ls", "[ls]"},
        {"echo a   b", "[echo][a][b]"},
        {"  cat  x  ", "[cat][x]"},
        // '...': everything literal up to the next '.
        {"echo 'a   b'", "[echo][a   b]"},
        {"echo 'x|y'", "[echo][x|y]"},
        {"echo 'a>b' '>>'", "[echo][a>b][>>]"},
        {"echo 'say \"hi\"' 'a\\b'", "[echo][say \"hi\"][a\\b]"},
        // "...": literal but \" and \; any other backslash is kept.
        {"echo \"a   b\"", "[echo][a   b]"},
        {"echo \"it's\"", "[echo][it's]"},
        {"echo \"a\\\"b\"", "[echo][a\"b]"},
        {"echo \"a\\\\b\"", "[echo][a\\b]"},
        {"echo \"a\\nb\"", "[echo][a\\nb]"},
        {"echo \"x|y > z\"", "[echo][x|y > z]"},
        // \x outside quotes: x literal.
        {"echo a\\ b", "[echo][a b]"},
        {"echo \\'x\\\"", "[echo]['x\"]"},
        {"echo a\\|b a\\>b \\\\", "[echo][a|b][a>b][\\]"},
        // Operators, with or without spaces.
        {"a|b", "[a]<|>[b]"},
        {"a | b", "[a]<|>[b]"},
        {"a>f", "[a]<>>[f]"},
        {"a>>f", "[a]<>>>[f]"},
        {"a >>> f", "[a]<>>><>>[f]"},
        {"a> >f", "[a]<>><>>[f]"},
        {"| grep x", "<|>[grep][x]"},
        {"> f", "<>>[f]"},
        {"a |", "[a]<|>"},
        {"a||b", "[a]<|><|>[b]"},
        {"dmesg | grep -i 'no route' | head 3 >> /d.txt",
         "[dmesg]<|>[grep][-i][no route]<|>[head][3]<>>>[/d.txt]"},
        // Quotes open and close inside a word; "" and '' are empty words.
        {"a\"b c\"d", "[ab cd]"},
        {"'a'\"b\"c\\ d", "[abc d]"},
        {"echo \"\" ''", "[echo][][]"},
        {"\"\"", "[]"},
        {"x''|''y", "[x]<|>[y]"},
        // Nothing else is special.
        {"echo $HOME ~ * # ; & < 2> `x` $(y)", "[echo][$HOME][~][*][#][;][&][<][2]<>>[`x`][$(y)]"},
        {"lua -e 'print(\"hi\")'", "[lua][-e][print(\"hi\")]"},
        {"wall \"don't\"", "[wall][don't]"},
        // Errors.
        {"wall don't", "!unmatched quote"},
        {"echo \"abc", "!unmatched quote"},
        {"echo 'abc\"", "!unmatched quote"},
        {"echo \"a\\\"", "!unmatched quote"},
        {"echo a\\", "!line ends in \\"},
        {"\\", "!line ends in \\"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *got = split(cases[i].line);
        if (strcmp(got, cases[i].want)) {
            fprintf(stderr, "  %s\n    got:      %s\n    expected: %s\n", cases[i].line, got,
                    cases[i].want);
            CHECK(false);
        }
    }
}

TEST(thirty_two_words_and_no_more) {
    char line[200] = "";
    for (int i = 0; i < SHELL_WORDS_MAX; i++)
        strcat(line, "w ");
    CHECK(split(line)[0] == '[');
    strcat(line, "x");
    CHECK(!strcmp(split(line), "!too many words"));
    line[0] = '\0';                                 // the 33rd an operator, no spaces
    for (int i = 0; i < SHELL_WORDS_MAX; i++)
        strcat(line, i ? "|w" : "w");               // 63 words
    CHECK(!strcmp(split(line), "!too many words"));
    line[0] = '\0';
    for (int i = 0; i < SHELL_WORDS_MAX / 2; i++)
        strcat(line, "w|");                         // exactly 32, the last a '|'
    CHECK(strcmp(split(line), "!too many words") != 0);
    strcat(line, "w");
    CHECK(!strcmp(split(line), "!too many words"));
}

// The output is written over the input and is never longer.
TEST(splits_in_place) {
    char line[] = "x \"a b\"|y";
    words_t w;
    CHECK(words_split(line, &w) == NULL);
    CHECK_EQ(w.n, 4);
    CHECK(w.v[0] == line && w.v[1] == line + 2);
    CHECK(!strcmp(w.v[1], "a b") && !strcmp(w.v[3], "y"));
    CHECK(w.v[2] == NULL && w.kind[2] == WORD_PIPE && w.v[4] == NULL);
}

static const char *join(const char *line) {
    static char buf[300];
    snprintf(buf, sizeof(buf), "%s", line);
    words_t w;
    CHECK(words_split(buf, &w) == NULL);
    int n = 0;
    while (n < w.n && w.v[n])
        n++;
    const char *got = words_join(n, w.v);
    return got ? got : "(null)";
}

TEST(join_gives_the_arguments_single_spaced) {
    CHECK(!strcmp(join("echo"), "(null)"));             // no argument
    CHECK(!strcmp(join("echo \"\""), "(null)"));        // only an empty one
    CHECK(!strcmp(join("echo \"\" \"\""), " "));        // two: the space between them
    CHECK(!strcmp(join("echo a   b"), "a b"));
    CHECK(!strcmp(join("echo \"a   b\""), "a   b"));
    CHECK(!strcmp(join("echo 'x|y'  z"), "x|y z"));
    CHECK(!strcmp(join("wall \"don't\" panic"), "don't panic"));
    CHECK(!strcmp(join("lua -e 'print(\"hi\")'"), "-e print(\"hi\")"));
    CHECK(!strcmp(join("echo a b | grep a"), "a b"));   // the command's words only
    CHECK(words_join(0, NULL) == NULL);
    // In place, starting at argv[1]; argv[0] is left alone.
    char line[] = "say \"one\"  'two'  three";
    words_t w;
    CHECK(words_split(line, &w) == NULL);
    char *args = words_join(w.n, w.v);
    CHECK(args == w.v[1] && !strcmp(args, "one two three") && !strcmp(w.v[0], "say"));
}

// words_at() on `line` as "start [text] quote escape after": quote '-' for none, escape 1 or 0,
// after one of ^ (nothing) w (a word) | (a pipe) > (a redirect).
static const char *at(const char *line) {
    static char out[600];
    char text[300];
    memset(text, 'X', sizeof(text));
    word_at_t a;
    size_t len = strlen(line);
    words_at(line, len, text, &a);
    CHECK(a.start <= len && a.len <= len - a.start && text[a.len] == '\0');
    snprintf(out, sizeof(out), "%zu [%s] %c %d %c", a.start, text, a.quote ? a.quote : '-',
             a.escape, "^w|>"[a.after]);
    return out;
}

TEST(at_reports_the_word_at_the_cursor) {
    static const struct {
        const char *line, *want;
    } cases[] = {
        {"", "0 [] - 0 ^"},
        {"  ", "2 [] - 0 ^"},
        {"ca", "0 [ca] - 0 ^"},
        {"  ca", "2 [ca] - 0 ^"},
        {"cat ", "4 [] - 0 w"},
        {"cat my", "4 [my] - 0 w"},
        // Quotes and backslashes: de-quoted, the open quote said.
        {"cat 'my f", "4 [my f] ' 0 w"},
        {"cat \"my f", "4 [my f] \" 0 w"},
        {"cat my\\ f", "4 [my f] - 0 w"},
        {"cat my\\", "4 [my] - 1 w"},
        {"cat \\", "4 [] - 1 w"},
        {"cat a'b c'd", "4 [ab cd] - 0 w"},
        {"cat 'it'\\''s", "4 [it's] ' 0 w"},
        {"cat \"a\\\"b\\\\c", "4 [a\"b\\c] \" 0 w"},
        {"cat \"a\\", "4 [a\\] \" 0 w"},       // inside "...": literal for now
        {"cat 'a\\", "4 [a\\] ' 0 w"},
        {"cat ''", "4 [] - 0 w"},
        {"cat '", "4 [] ' 0 w"},
        // Operators: unquoted ones end the word and say what the next one is.
        {"ls | gr", "5 [gr] - 0 |"},
        {"ls|gr", "3 [gr] - 0 |"},
        {"ls |", "4 [] - 0 |"},
        {"ls | grep x", "10 [x] - 0 w"},
        {"echo hi > do", "10 [do] - 0 >"},
        {"echo hi >> do", "11 [do] - 0 >"},
        {"echo hi>do", "8 [do] - 0 >"},
        {"echo hi >", "9 [] - 0 >"},
        {"echo hi > f x", "12 [x] - 0 w"},
        // Quoted or escaped operators are text.
        {"echo \"a>b\" > do", "13 [do] - 0 >"},
        {"echo \"a>b\" do", "11 [do] - 0 w"},
        {"echo 'a|b' | gr", "13 [gr] - 0 |"},
        {"echo 'a|b' gr", "11 [gr] - 0 w"},
        {"echo 'a > b", "5 [a > b] ' 0 w"},
        {"echo a\\>b", "5 [a>b] - 0 w"},
        {"echo a\\|b", "5 [a|b] - 0 w"},
        {"echo \"x | y", "5 [x | y] \" 0 w"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *got = at(cases[i].line);
        if (strcmp(got, cases[i].want)) {
            fprintf(stderr, "  %s\n    got:      %s\n    expected: %s\n", cases[i].line, got,
                    cases[i].want);
            CHECK(false);
        }
    }
}

// The words before the cursor's word are whole: words_split() takes them, and the word, its
// quote closed, is what words_split() makes of it.
TEST(at_agrees_with_split) {
    static const char *const lines[] = {
        "cat 'my f", "cat \"a\\\"b", "x | y 'a b", "a\"b c\"d", "echo \"a>b\" > 'do", "w  ",
        "e a\\ b",
    };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        char text[300], buf[300];
        word_at_t a;
        size_t len = strlen(lines[i]);
        words_at(lines[i], len, text, &a);
        memcpy(buf, lines[i], a.start);
        buf[a.start] = '\0';
        words_t w;
        CHECK(words_split(buf, &w) == NULL);
        char close[2] = {a.quote, '\0'};
        snprintf(buf, sizeof(buf), "%s%s", lines[i] + a.start, close);
        CHECK(words_split(buf, &w) == NULL);
        CHECK(w.n == (a.len || a.start < len ? 1 : 0));
        CHECK(!w.n || !strcmp(w.v[0], text));
    }
}

TEST_MAIN(
    RUN(splits_every_rule);
    RUN(thirty_two_words_and_no_more);
    RUN(splits_in_place);
    RUN(join_gives_the_arguments_single_spaced);
    RUN(at_reports_the_word_at_the_cursor);
    RUN(at_agrees_with_split);
)
