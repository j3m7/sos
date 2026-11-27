// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// src/shell/words.c: a line typed at the shell (or pasted, or sent over SSH), split into words
// in place, then each command or stage segment joined as shell_execute() joins it; and the word
// Tab would complete at its end.

#include <stdlib.h>
#include <string.h>

#include "fuzz.h"
#include "shell/words.h"

// Checked in every build (NDEBUG or not).
#define MUST(x) do { if (!(x)) abort(); } while (0)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char line[1024];
    if (size >= sizeof(line))
        size = sizeof(line) - 1;
    memcpy(line, data, size);
    line[size] = '\0';
    size_t len = strlen(line);              // the line stops at a NUL, as the shell's does
    // Tab's view (words_at()) of the line up to its end: the word is inside it, its text no
    // longer, and the words before it split without an error but "too many words".
    char text[1024], before[1024];
    word_at_t at;
    words_at(line, len, text, &at);
    MUST(at.start <= len && at.len <= len - at.start && text[at.len] == '\0');
    MUST(at.after <= WORD_AFTER_REDIRECT);
    MUST(at.quote == '\0' || at.quote == '\'' || at.quote == '"');
    memcpy(before, line, at.start);
    before[at.start] = '\0';
    words_t w;
    const char *why = words_split(before, &w);
    MUST(!why || !strcmp(why, "too many words"));
    if (at.start < len && !at.quote && !at.escape) {    // a whole word: as words_split() has it
        memcpy(before, line + at.start, len - at.start + 1);
        MUST(words_split(before, &w) == NULL && w.n == 1 && !strcmp(w.v[0], text));
    }
    if (words_split(line, &w))
        return 0;
    MUST(w.n >= 0 && w.n <= SHELL_WORDS_MAX);
    MUST(w.v[w.n] == NULL);
    const char *prev_end = line;            // the words lie in order, without overlapping
    size_t out = 0;
    for (int i = 0; i < w.n; i++) {
        if (w.kind[i] != WORD_PLAIN) {
            MUST(w.kind[i] == WORD_PIPE || w.kind[i] == WORD_REDIRECT ||
                   w.kind[i] == WORD_APPEND);
            MUST(w.v[i] == NULL);
            continue;
        }
        MUST(w.v[i] != NULL);
        MUST(w.v[i] >= prev_end && w.v[i] <= line + len);
        const char *nul = memchr(w.v[i], '\0', (size_t)(line + len + 1 - w.v[i]));
        MUST(nul != NULL);                // NUL-terminated inside the buffer
        prev_end = nul + 1;
        out += (size_t)(nul - w.v[i]) + 1;
    }
    MUST(out <= len + 1);                 // the output is never longer than the input
    // Join each segment, as shell_execute() does for its command.
    for (int i = 0; i < w.n;) {
        int n = 0;
        while (i + n < w.n && w.v[i + n])
            n++;
        if (n) {
            size_t want = 0;
            for (int j = 1; j < n; j++)
                want += strlen(w.v[i + j]) + (j > 1);
            char *joined = words_join(n, &w.v[i]);
            MUST(want ? joined && strlen(joined) == want : !joined);
        }
        i += n ? n : 1;
    }
    return 0;
}
