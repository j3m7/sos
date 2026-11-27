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

TEST_MAIN(
    RUN(splits_every_rule);
    RUN(thirty_two_words_and_no_more);
    RUN(splits_in_place);
    RUN(join_gives_the_arguments_single_spaced);
)
