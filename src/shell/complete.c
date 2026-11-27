// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Tab completion over the command tables and the SD card's directories (see complete.h).

#include "shell/complete.h"
#include "shell/termtext.h"

#include <stdint.h>
#include <string.h>

static bool offered(unsigned flags, bool admin) {
    return !(flags & SHELL_HIDDEN) && (admin || !(flags & SHELL_ADMIN));
}

// Is line[0..n) exactly `name`?
static bool is(const char *name, const char *word, size_t n) {
    return !strncmp(name, word, n) && name[n] == '\0';
}

// A space that isn't U+0020: it reads as a word break on the line but isn't one.
static bool unicode_space(uint32_t cp) {
    return cp == 0xa0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x202f ||
           cp == 0x205f || cp == 0x3000;
}

// Can a file name go on the line as one word, and be shown on the terminal as it is? Valid
// UTF-8 (no overlong forms, surrogates or code points past U+10FFFF) from U+0021 up, without
// DEL and the C1 controls (U+0080-U+009F): a name can't send the terminal escape sequences.
// Nor what termtext_hidden() names (bidi controls, zero-width and other invisible characters,
// tag characters, variation selectors) or other spaces: Tab puts the name on the line about to
// be run, so what it shows must be what it is, and two names must never look the same.
// With `spaces`, U+0020 may be in it too (an answer: an SSID can hold spaces), but only between
// other characters and one at a time: "abc " would look like "abc", and "a  b" like "a b".
static bool text_ok(const char *name, bool spaces) {
    const unsigned char *p = (const unsigned char *)name;
    if (!*p)
        return false;
    if (spaces) {
        size_t n = strlen(name);
        if (name[0] == ' ' || name[n - 1] == ' ' || strstr(name, "  "))
            return false;
    }
    while (*p) {
        unsigned c = *p;
        if (c < 0x80) {
            if ((c <= 0x20 && !(spaces && c == ' ')) || c == 0x7f)
                return false;
            p++;
            continue;
        }
        unsigned n;
        uint32_t cp, min;
        if ((c & 0xe0) == 0xc0) {
            n = 1, cp = c & 0x1f, min = 0x80;
        } else if ((c & 0xf0) == 0xe0) {
            n = 2, cp = c & 0x0f, min = 0x800;
        } else if ((c & 0xf8) == 0xf0) {
            n = 3, cp = c & 0x07, min = 0x10000;
        } else {
            return false;
        }
        for (unsigned i = 1; i <= n; i++) {
            if ((p[i] & 0xc0) != 0x80)
                return false;
            cp = cp << 6 | (p[i] & 0x3f);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || cp < 0xa0 ||
            termtext_hidden(cp) || unicode_space(cp))
            return false;
        p += n + 1;
    }
    return true;
}

// Does `name` start with prefix[0..n), ignoring the case of ASCII letters? FAT matches names
// that way too (README.TXT is readme.txt); other letters must be the same.
static bool starts_with_any_case(const char *name, const char *prefix, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char a = (unsigned char)name[i], b = (unsigned char)prefix[i];
        if (a >= 'A' && a <= 'Z')
            a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z')
            b += 'a' - 'A';
        if (a != b || !a)
            return false;
    }
    return true;
}

typedef struct {
    const char *prefix;
    size_t plen;
    bool dirs_only;
    complete_fn fn;
    void *arg;
    size_t count;
} entries_t;

static void take_entry(const char *name, bool is_dir, void *arg) {
    entries_t *e = arg;
    if ((e->dirs_only && !is_dir) || !starts_with_any_case(name, e->prefix, e->plen) ||
        !text_ok(name, false) || strlen(name) > SHELL_LINE_MAX)   // too long for the line anyway
        return;
    e->fn(name, is_dir ? '/' : ' ', NULL, e->arg);
    e->count++;
}

// Complete the path line[p..len): the entries of the directory it names up to its last '/' that
// start with what follows. *start goes just after that '/'.
static size_t complete_path(const complete_src_t *src, const char *line, size_t p, size_t len,
                            bool dirs_only, size_t *start, complete_fn fn, void *arg,
                            bool *partial) {
    if (!src->list_dir)
        return 0;
    const char *prefix = line + p;
    size_t plen = len - p;
    size_t slash = plen;
    while (slash > 0 && prefix[slash - 1] != '/')
        slash--;
    char dir[SHELL_LINE_MAX + 1];
    memcpy(dir, prefix, slash);
    dir[slash] = '\0';
    entries_t e = {prefix + slash, plen - slash, dirs_only, fn, arg, 0};
    complete_dir_t r = src->list_dir(src->sh, dir, take_entry, &e);
    *start = p + slash;
    *partial = r == COMPLETE_DIR_PART;
    return e.count;
}

// A command marked SHELL_REDIRECT (echo) writes to the text after the line's last '>' (or ">>"),
// spaces before it skipped: that's a path when the cursor is in it. Complete it then; nothing
// otherwise (the words before are free text).
static size_t complete_redirect(const complete_src_t *src, const char *line, size_t from,
                                size_t len, size_t *start, complete_fn fn, void *arg,
                                bool *partial) {
    size_t gt = len;
    while (gt > from && line[gt - 1] != '>')
        gt--;
    if (gt == from)
        return 0;
    size_t p = gt;
    while (p < len && line[p] == ' ')
        p++;
    if (memchr(line + p, ' ', len - p))  // a name with a space: never offered (complete.h)
        return 0;
    return complete_path(src, line, p, len, false, start, fn, arg, partial);
}

typedef struct {
    const char *prefix;
    size_t plen;
    complete_fn fn;
    void *arg;
    size_t count;
} values_t;

// A word from a values function: offered like a file name, only if it can go on the line as
// one word and be shown as it is.
static void take_value(const char *value, const char *note, void *arg) {
    values_t *v = arg;
    if (strncmp(value, v->prefix, v->plen) || !text_ok(value, false) ||
        strlen(value) > SHELL_LINE_MAX)
        return;
    v->fn(value, ' ', note, v->arg);
    v->count++;
}

static bool is_values(const shell_word_t *words) {
    return words && !words->name && (words->flags & SHELL_VALUES);
}

static char word_end(unsigned flags) {
    return flags & SHELL_NOSPACE ? '\0' : ' ';
}

// After a '|': the words there are a stage's (pipe.c), when the first of them is a stage name or
// is the word being typed. Then complete them as a line of their own, from pipe_stage_table (it
// has no '|' in it, so this doesn't come back here).
static bool complete_stage(const complete_src_t *src, const char *line, size_t len,
                           size_t *start, complete_fn fn, void *arg, bool *partial,
                           size_t *count) {
    size_t bar = len;
    while (bar > 0 && line[bar - 1] != '|')
        bar--;
    if (bar == 0)
        return false;
    size_t w = bar;
    while (w < len && line[w] == ' ')
        w++;
    size_t e = w;
    while (e < len && line[e] != ' ')
        e++;
    if (e < len) {                      // a word before the one being typed: a stage's name?
        bool stage = false;
        for (const shell_cmd_t *c = pipe_stage_table; c->name && !stage; c++)
            stage = is(c->name, line + w, e - w);
        if (!stage)
            return false;
    }
    static const shell_cmd_t *const tables[] = {pipe_stage_table, NULL};
    complete_src_t sub = *src;
    sub.tables = tables;
    *count = complete_words(&sub, line + bar, len - bar, start, fn, arg, partial);
    *start += bar;
    return true;
}

size_t complete_words(const complete_src_t *src, const char *line, size_t len, size_t *start,
                      complete_fn fn, void *arg, bool *partial) {
    *partial = false;
    size_t stage_count;
    if (complete_stage(src, line, len, start, fn, arg, partial, &stage_count))
        return stage_count;
    size_t s = len;
    while (s > 0 && line[s - 1] != ' ')
        s--;
    *start = s;

    // Walk the words before the last one down the tables. `words` are those that may come next;
    // with `paths`, anything else is a path too.
    const shell_word_t *words = NULL;
    bool at_command = true, paths = false, dirs_only = false;
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
        unsigned flags;
        if (at_command) {
            const shell_cmd_t *found = NULL;
            for (const shell_cmd_t *const *t = src->tables; *t && !found; t++)
                for (const shell_cmd_t *c = *t; c->name; c++)
                    if (is(c->name, word, n)) {
                        found = c;
                        break;
                    }
            if (!found || (!src->admin && (found->flags & SHELL_ADMIN)))
                return 0;
            if (found->flags & SHELL_REDIRECT)
                return complete_redirect(src, line, i, len, start, fn, arg, partial);
            words = found->words;
            flags = found->flags;
            at_command = false;
        } else {
            const shell_word_t *found = NULL;
            for (const shell_word_t *x = words; x && x->name; x++)
                if (is(x->name, word, n)) {
                    found = x;
                    break;
                }
            if (!found) {
                if (!paths)
                    return 0;
                words = NULL;           // a path: more paths may follow, no more words
                continue;
            }
            words = found->words;
            flags = found->flags;
        }
        if (!src->admin && (flags & SHELL_ADMIN))
            return 0;
        paths = flags & SHELL_PATH;
        dirs_only = flags & SHELL_DIR;
        if (!words && !paths)
            return 0;
    }

    size_t count = 0;
    const char *prefix = line + s;
    size_t plen = len - s;
    if (at_command) {
        for (const shell_cmd_t *const *t = src->tables; *t; t++)
            for (const shell_cmd_t *c = *t; c->name; c++)
                if (offered(c->flags, src->admin) && !strncmp(c->name, prefix, plen)) {
                    fn(c->name, word_end(c->flags), NULL, arg);
                    count++;
                }
        return count;
    }
    if (is_values(words)) {
        values_t v = {prefix, plen, fn, arg, 0};
        ((const shell_values_t *)words)->list(src->sh, take_value, &v);
        count = v.count;
    }
    for (const shell_word_t *x = words; x && x->name; x++)
        if (offered(x->flags, src->admin) && !strncmp(x->name, prefix, plen)) {
            fn(x->name, word_end(x->flags), NULL, arg);
            count++;
        }
    if (!paths)
        return count;
    // With a '/' in the path, *start moves past it, but then no word matched (words have none).
    return count + complete_path(src, line, s, len, dirs_only, start, fn, arg, partial);
}

typedef struct {
    const char *typed;
    size_t len;
    bool words;
    char end;
    complete_fn fn;
    void *arg;
    size_t count;
} answers_t;

static void take_answer(const char *value, const char *note, void *arg) {
    answers_t *a = arg;
    if (strncmp(value, a->typed, a->len) || !text_ok(value, !a->words) ||
        strlen(value) > SHELL_LINE_MAX)
        return;
    a->fn(value, a->end, note, a->arg);
    a->count++;
}

size_t complete_answer(shell_t *sh, shell_ask_complete_fn list, const char *line, size_t len,
                       bool words, const char *chars, size_t *start, complete_fn fn, void *arg,
                       bool *partial) {
    size_t s = 0;
    if (words && chars)
        for (s = len; s > 0 && strchr(chars, line[s - 1]); s--)
            ;
    else if (words)
        for (s = len; s > 0 && line[s - 1] != ' '; s--)
            ;
    *start = s;
    // A whole answer is done once completed (Enter sends it); a word between spaces gets a
    // space, as on the command line; a word of `chars` (a Lua name) gets nothing: what follows
    // it is up to the person ('.', '(', ...).
    answers_t a = {line + s, len - s, words, (char)(!words ? '\0' : chars ? '\0' : ' '), fn, arg,
                   0};
    *partial = list(sh, take_answer, &a);
    return a.count;
}
