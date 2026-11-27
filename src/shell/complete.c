// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Tab completion over the command tables (see complete.h).

#include "shell/complete.h"

#include <string.h>

static bool offered(unsigned flags, bool admin) {
    return !(flags & SHELL_HIDDEN) && (admin || !(flags & SHELL_ADMIN));
}

// Is line[0..n) exactly `name`?
static bool is(const char *name, const char *word, size_t n) {
    return !strncmp(name, word, n) && name[n] == '\0';
}

size_t complete_words(const shell_cmd_t *const *tables, bool admin, const char *line, size_t len,
                      size_t *start, complete_fn fn, void *arg) {
    size_t s = len;
    while (s > 0 && line[s - 1] != ' ')
        s--;
    *start = s;
    const char *prefix = line + s;
    size_t plen = len - s;

    // Walk the words before the last one down the tables.
    const shell_word_t *words = NULL;
    bool at_command = true;
    size_t i = 0;
    for (;;) {
        while (i < s && line[i] == ' ')
            i++;
        if (i >= s)
            break;
        size_t w = i;
        while (i < s && line[i] != ' ')
            i++;
        const char *word = line + w;
        size_t n = i - w;
        if (at_command) {
            const shell_cmd_t *found = NULL;
            for (const shell_cmd_t *const *t = tables; *t && !found; t++)
                for (const shell_cmd_t *c = *t; c->name; c++)
                    if (is(c->name, word, n)) {
                        found = c;
                        break;
                    }
            if (!found || (!admin && (found->flags & SHELL_ADMIN)))
                return 0;
            words = found->words;
            at_command = false;
        } else {
            const shell_word_t *found = NULL;
            for (const shell_word_t *x = words; x && x->name; x++)
                if (is(x->name, word, n)) {
                    found = x;
                    break;
                }
            if (!found || (!admin && (found->flags & SHELL_ADMIN)))
                return 0;
            words = found->words;
        }
        if (!words && !at_command)
            return 0;
    }

    size_t count = 0;
    if (at_command) {
        for (const shell_cmd_t *const *t = tables; *t; t++)
            for (const shell_cmd_t *c = *t; c->name; c++)
                if (offered(c->flags, admin) && !strncmp(c->name, prefix, plen)) {
                    fn(c->name, arg);
                    count++;
                }
    } else {
        for (const shell_word_t *x = words; x->name; x++)
            if (offered(x->flags, admin) && !strncmp(x->name, prefix, plen)) {
                fn(x->name, arg);
                count++;
            }
    }
    return count;
}
