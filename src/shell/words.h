// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_WORDS_H
#define SOS_WORDS_H

// The shell's words: a typed line split into words, quotes and backslashes removed, before
// anything else reads it (docs/design/shell-quoting.md §2). Pure C, no heap: it works in the
// line's own buffer, so it builds on the host for unit tests and a fuzz target.
//
// Rules, a subset of POSIX sh word splitting:
// - Words are separated by one or more spaces.
// - '...': everything up to the next ' is literal, including ", \, |, > and spaces.
// - "...": literal, except that \" is " and \\ is \; any other backslash is kept as it is.
// - \x outside quotes: x is literal, whatever it is (a space, a quote, |, >, \).
// - |, > and >> outside quotes are operators. They end the word before them and need no spaces
//   ("a|b" is a, |, b); ">>" is one operator, ">>>" is ">>" then ">".
// - Quotes may open and close inside a word: a"b c"d is the one word "ab cd"; "" and '' are a
//   word that is the empty string.
// - Nothing else is special: no variables, globbing, ~, #, ;, &, <, backquotes or $(...).
// - An unmatched quote, a line ending in an unescaped backslash, or a 33rd word is an error.

#define SHELL_WORDS_MAX 32                      // words in a line, operators included

typedef enum { WORD_PLAIN, WORD_PIPE, WORD_REDIRECT, WORD_APPEND } word_kind_t;

typedef struct {
    // The words; NULL after the last and for operators, so that every command and stage segment
    // (the plain words between two operators) is a NULL-terminated slice of v that can be
    // passed as argv without copying.
    char *v[SHELL_WORDS_MAX + 1];
    unsigned char kind[SHELL_WORDS_MAX];        // word_kind_t
    int n;                                      // words, operators included
} words_t;

// Split `line` in place. Returns NULL, or why not: "unmatched quote", "line ends in \",
// "too many words". `line` is unusable after an error (partly rewritten), and so is `w`.
//
// The contract callers rely on: every plain word is NUL-terminated inside the input's bytes
// (the output is never longer than the input), and the words lie in that buffer in line order
// without overlapping: v[i] + strlen(v[i]) < v[j] for every plain i < j. So the words can be
// joined in place (words_join()) by moving bytes left only.
const char *words_split(char *line, words_t *w);

// The arguments of a command as one string, for handlers that take free text (`echo`, `wall`,
// `lua -e`): argv[1..argc) joined with single spaces, in place, into the buffer argv[1] starts
// at. The words must be words_split()'s, in line order, with no operator among them (a command
// or stage segment: argv[argc] is NULL). Quoted spaces stay, being inside a word: `echo "a   b"`
// gives "a   b", `echo a   b` gives "a b", `echo "" ""` gives " ". Returns NULL when there is no
// argument (argc < 2) or the joined text is empty (`echo ""`), else the text as it is. argv[2]
// and later are clobbered.
char *words_join(int argc, char **argv);

#endif
