// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The shell's tokenizer: a typed line into words, in place (see words.h and
// docs/design/shell-quoting.md §2).

#include "shell/words.h"

#include <stddef.h>
#include <string.h>

// The operator at `in` (|, > or >>) is word i: note it, and return the bytes it takes.
static size_t take_operator(const char *in, words_t *w, int i) {
    w->kind[i] = in[0] == '|' ? WORD_PIPE : in[1] == '>' ? WORD_APPEND : WORD_REDIRECT;
    w->v[i] = NULL;
    return w->kind[i] == WORD_APPEND ? 2 : 1;
}

const char *words_split(char *line, words_t *w) {
    w->n = 0;
    w->v[0] = NULL;
    const char *in = line;      // reads ahead of `out`, which writes the de-quoted words
    char *out = line;
    for (;;) {
        while (*in == ' ')
            in++;
        if (!*in)
            break;
        if (w->n == SHELL_WORDS_MAX)
            return "too many words";
        int i = w->n++;
        if (*in == '|' || *in == '>') {
            in += take_operator(in, w, i);
            continue;
        }
        w->kind[i] = WORD_PLAIN;
        w->v[i] = out;
        // One word: up to an unquoted space, operator or the end.
        while (*in && *in != ' ' && *in != '|' && *in != '>') {
            char c = *in++;
            if (c == '\\') {
                if (!*in)
                    return "line ends in \\";
                *out++ = *in++;
            } else if (c == '\'') {
                while (*in && *in != '\'')
                    *out++ = *in++;
                if (!*in)
                    return "unmatched quote";
                in++;
            } else if (c == '"') {
                while (*in && *in != '"') {
                    if (*in == '\\' && (in[1] == '"' || in[1] == '\\'))
                        in++;
                    *out++ = *in++;
                }
                if (!*in)
                    return "unmatched quote";
                in++;
            } else {
                *out++ = c;
            }
        }
        // Its terminator goes at `out`, which never passes `in`. When nothing has been removed
        // from the line yet they are equal, and the byte there (a space or an operator: "a|b")
        // hasn't been dealt with: take it first.
        if (out == in && *in == ' ') {
            in++;
        } else if (out == in && *in) {
            if (w->n == SHELL_WORDS_MAX)
                return "too many words";
            in += take_operator(in, w, w->n++);
        }
        *out++ = '\0';
    }
    w->v[w->n] = NULL;
    return NULL;
}

char *words_join(int argc, char **argv) {
    if (argc < 2)
        return NULL;
    char *out = argv[1] + strlen(argv[1]);
    for (int i = 2; i < argc; i++) {
        size_t len = strlen(argv[i]);
        *out++ = ' ';
        memmove(out, argv[i], len);     // left, or not at all: argv[i] lies after `out`
        out += len;
    }
    *out = '\0';
    return *argv[1] ? argv[1] : NULL;
}
