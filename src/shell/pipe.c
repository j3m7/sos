// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Pipelines: the stage parser and the filters grep, head, tail, wc and tee (see pipe.h and
// docs/design/shell-pipes.md). shell_core.c supplies the sink and routes the producer's output
// here.

#include "shell/pipe.h"
#include "core/heaptag.h"
#include "shell/complete.h"
#include "shell/shell.h"

#include <stdio.h>
#include <string.h>

// --- the chain -------------------------------------------------------------------------------

void pipe_emit(pipe_stage_t *st, const char *buf, size_t len) {
    if (!len)
        return;
    if (st->next)
        st->next->f->write(st->next, buf, len);
    else
        st->pipe->out(st->pipe->sh, buf, len);
}

void pipe_shut(pipe_stage_t *st) {
    st->pipe->done = true;
}

void pipe_write(pipeline_t *p, const char *buf, size_t len) {
    if (p->done || !len)
        return;
    if (p->first)
        p->first->f->write(p->first, buf, len);
    else
        p->out(p->sh, buf, len);
}

static void close_all(pipeline_t *p) {
    pipe_stage_t *st = p->first;
    p->first = NULL;
    p->done = false;
    while (st) {
        pipe_stage_t *next = st->next;
        st->f->close(st);
        st = next;
    }
}

void pipe_end(pipeline_t *p) {
    // In order: what a stage flushes goes through the stages after it, which haven't ended yet.
    for (pipe_stage_t *st = p->first; st; st = st->next)
        st->f->end(st);
    close_all(p);
}

void pipe_close(pipeline_t *p) {
    close_all(p);
}

// A filter's state, zeroed; NULL after saying so when memory is short.
static void *stage_new(shell_t *sh, const pipe_filter_t *f, size_t size) {
    pipe_stage_t *st = heap_zalloc(HEAP_EDIT, size);
    if (!st) {
        shell_printf(sh, "%s: out of memory\n", f->name);
        return NULL;
    }
    st->f = f;
    return st;
}

static void stage_free(pipe_stage_t *st) {
    heap_release(HEAP_EDIT, st);
}

static void end_nothing(pipe_stage_t *st) {
    (void)st;
}

// A count of lines for head and tail: 10 without one, else 1 to 999999.
static bool parse_lines(const char *args, unsigned long *n) {
    if (!args) {
        *n = 10;
        return true;
    }
    unsigned long v = 0;
    size_t i = 0;
    for (; args[i] >= '0' && args[i] <= '9'; i++) {
        if (i == 6)
            return false;
        v = v * 10 + (unsigned long)(args[i] - '0');
    }
    if (i == 0 || args[i] || v == 0)
        return false;
    *n = v;
    return true;
}

// --- grep [-v] [-i] [-n] <text> ----------------------------------------------------------------

typedef struct {
    pipe_stage_t st;
    bool invert, nocase, number;
    bool cont;                  // the piece being read continues a line cut at PIPE_LINE_MAX
    unsigned long line;         // the number of the line being read (for -n)
    size_t plen, len;
    char pattern[PIPE_PATTERN_MAX];
    char buf[PIPE_LINE_MAX];
} grep_t;

static const pipe_filter_t grep_filter;

static char fold(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}

static bool contains(const grep_t *g) {
    for (size_t i = 0; i + g->plen <= g->len; i++) {
        size_t j = 0;
        if (g->nocase)
            while (j < g->plen && fold(g->buf[i + j]) == g->pattern[j])
                j++;
        else
            while (j < g->plen && g->buf[i + j] == g->pattern[j])
                j++;
        if (j == g->plen)
            return true;
    }
    return false;
}

// The line (or piece of one) in buf is complete: pass it on, with a newline, if it matches.
static void grep_line(grep_t *g) {
    if (!g->cont)
        g->line++;
    if (contains(g) != g->invert) {
        if (g->number) {
            char num[16];
            int n = snprintf(num, sizeof(num), "%lu:", g->line);
            pipe_emit(&g->st, num, (size_t)n);
        }
        pipe_emit(&g->st, g->buf, g->len);
        pipe_emit(&g->st, "\n", 1);
    }
    g->len = 0;
}

static void grep_write(pipe_stage_t *st, const char *buf, size_t len) {
    grep_t *g = (grep_t *)st;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            if (!g->cont || g->len)         // not the empty rest of a line cut at a piece's end
                grep_line(g);
            g->cont = false;
            continue;
        }
        if (g->len == sizeof(g->buf)) {     // a long line: this piece is matched on its own
            grep_line(g);
            g->cont = true;
        }
        g->buf[g->len++] = buf[i];
    }
}

static void grep_end(pipe_stage_t *st) {
    grep_t *g = (grep_t *)st;
    if (g->len)                             // a last line without a newline
        grep_line(g);
}

static pipe_stage_t *grep_open(shell_t *sh, const char *args) {
    bool invert = false, nocase = false, number = false;
    const char *a = args ? args : "";
    // Words of option letters come first; the first other word starts the text, which runs to
    // the end, spaces kept.
    while (a[0] == '-' && a[1] && a[1] != ' ') {
        size_t n = 1;
        while (a[n] == 'v' || a[n] == 'i' || a[n] == 'n')
            n++;
        if (a[n] && a[n] != ' ')
            break;                          // not options: the text begins with '-'
        invert |= memchr(a, 'v', n) != NULL;
        nocase |= memchr(a, 'i', n) != NULL;
        number |= memchr(a, 'n', n) != NULL;
        a += n;
        while (*a == ' ')
            a++;
    }
    size_t plen = strlen(a);
    if (!plen) {
        shell_printf(sh, "usage: <command> | grep [-v] [-i] [-n] <text>\n");
        return NULL;
    }
    if (plen > PIPE_PATTERN_MAX) {
        shell_printf(sh, "grep: the text is longer than %d bytes\n", PIPE_PATTERN_MAX);
        return NULL;
    }
    grep_t *g = stage_new(sh, &grep_filter, sizeof(grep_t));
    if (!g)
        return NULL;
    g->invert = invert;
    g->nocase = nocase;
    g->number = number;
    g->plen = plen;
    for (size_t i = 0; i < plen; i++)
        g->pattern[i] = nocase ? fold(a[i]) : a[i];
    return &g->st;
}

static const pipe_filter_t grep_filter = {"grep", grep_open, grep_write, grep_end, stage_free};

// --- head [<n>] ------------------------------------------------------------------------------

typedef struct {
    pipe_stage_t st;
    unsigned long left;         // lines still to pass
} head_t;

static const pipe_filter_t head_filter;

static void head_write(pipe_stage_t *st, const char *buf, size_t len) {
    head_t *h = (head_t *)st;
    if (!h->left)
        return;
    size_t n = 0;
    while (n < len && h->left)
        if (buf[n++] == '\n')
            h->left--;
    pipe_emit(st, buf, n);
    if (!h->left)
        pipe_shut(st);
}

static pipe_stage_t *head_open(shell_t *sh, const char *args) {
    unsigned long n;
    if (!parse_lines(args, &n)) {
        shell_printf(sh, "usage: <command> | head [<lines>]  (1 to 999999, default 10)\n");
        return NULL;
    }
    head_t *h = stage_new(sh, &head_filter, sizeof(head_t));
    if (h)
        h->left = n;
    return h ? &h->st : NULL;
}

static const pipe_filter_t head_filter = {"head", head_open, head_write, end_nothing, stage_free};

// --- tail [<n>] ------------------------------------------------------------------------------

typedef struct {
    pipe_stage_t st;
    unsigned long want;         // lines to keep
    unsigned long newlines;     // '\n's in the ring
    size_t start, len;          // the ring's bytes: ring[start], ... (wrapping), len of them
    char ring[TAIL_MAX];
} tail_t;

static const pipe_filter_t tail_filter;

// Drop the ring's first line, its newline included (all of it when there is no newline).
static void tail_drop_line(tail_t *t) {
    size_t n = 0;
    while (n < t->len && t->ring[(t->start + n) % TAIL_MAX] != '\n')
        n++;
    if (n < t->len) {
        n++;
        t->newlines--;
    }
    t->start = (t->start + n) % TAIL_MAX;
    t->len -= n;
}

static void tail_write(pipe_stage_t *st, const char *buf, size_t len) {
    tail_t *t = (tail_t *)st;
    for (size_t i = 0; i < len; i++) {
        if (t->len == TAIL_MAX) {           // full: make room by whole lines
            if (t->newlines) {
                tail_drop_line(t);
            } else {                        // one line longer than the ring: keep its end
                t->start = (t->start + 1) % TAIL_MAX;
                t->len--;
            }
        }
        char c = buf[i];
        t->ring[(t->start + t->len) % TAIL_MAX] = c;
        t->len++;
        if (c == '\n')
            t->newlines++;
        // The lines held: each newline ends one, and a last line without one counts too.
        unsigned long lines = t->newlines + (c != '\n');
        for (; lines > t->want; lines--)
            tail_drop_line(t);
    }
}

static void tail_end(pipe_stage_t *st) {
    tail_t *t = (tail_t *)st;
    size_t first = TAIL_MAX - t->start < t->len ? TAIL_MAX - t->start : t->len;
    pipe_emit(st, t->ring + t->start, first);
    pipe_emit(st, t->ring, t->len - first);
    t->len = 0;
}

static pipe_stage_t *tail_open(shell_t *sh, const char *args) {
    unsigned long n;
    if (!parse_lines(args, &n)) {
        shell_printf(sh, "usage: <command> | tail [<lines>]  (1 to 999999, default 10)\n");
        return NULL;
    }
    tail_t *t = stage_new(sh, &tail_filter, sizeof(tail_t));
    if (t)
        t->want = n;
    return t ? &t->st : NULL;
}

static const pipe_filter_t tail_filter = {"tail", tail_open, tail_write, tail_end, stage_free};

// --- wc --------------------------------------------------------------------------------------

typedef struct {
    pipe_stage_t st;
    unsigned long lines, words, bytes;
    bool in_word;
} wc_t;

static const pipe_filter_t wc_filter;

static void wc_write(pipe_stage_t *st, const char *buf, size_t len) {
    wc_t *w = (wc_t *)st;
    for (size_t i = 0; i < len; i++) {
        char c = buf[i];
        bool space = c == ' ' || (c >= '\t' && c <= '\r');     // as isspace() in the C locale
        if (c == '\n')
            w->lines++;
        if (!space && !w->in_word)
            w->words++;
        w->in_word = !space;
    }
    w->bytes += len;
}

static void wc_end(pipe_stage_t *st) {
    wc_t *w = (wc_t *)st;
    char out[48];
    int n = snprintf(out, sizeof(out), "%lu %lu %lu\n", w->lines, w->words, w->bytes);
    pipe_emit(st, out, (size_t)n);
}

static pipe_stage_t *wc_open(shell_t *sh, const char *args) {
    if (args) {
        shell_printf(sh, "usage: <command> | wc\n");
        return NULL;
    }
    wc_t *w = stage_new(sh, &wc_filter, sizeof(wc_t));
    return w ? &w->st : NULL;
}

static const pipe_filter_t wc_filter = {"wc", wc_open, wc_write, wc_end, stage_free};

// --- tee [-a] <path> ------------------------------------------------------------------------

// The card side, unless files.c (or a test) supplies it: no card, nothing opens.
__attribute__((weak)) pipe_file_t *pipe_file_open(shell_t *sh, const char *path, bool append) {
    (void)append;
    shell_printf(sh, "tee: %s: no SD card\n", path);
    return NULL;
}

__attribute__((weak)) const char *pipe_file_write(pipe_file_t *f, const char *buf, size_t len) {
    (void)f;
    (void)buf;
    (void)len;
    return "no SD card";
}

__attribute__((weak)) const char *pipe_file_close(pipe_file_t *f) {
    (void)f;
    return NULL;
}

typedef struct {
    pipe_stage_t st;
    pipe_file_t *file;          // NULL once closed (or after a write error)
    char path[SHELL_PATH_MAX + 1];  // as typed, for the messages
} tee_t;

static const pipe_filter_t tee_filter;

// Say why, down the line like any output of the command's (there is no stderr; under `| more`
// it lands in the pager's text, not on top of its frame). Only once the file is closed, so the
// message isn't written into it.
static void tee_say(tee_t *t, const char *what) {
    char msg[SHELL_PATH_MAX + 80];
    int n = snprintf(msg, sizeof(msg), "tee: %s: %s\n", t->path, what);
    if (n > 0)
        pipe_emit(&t->st, msg, (size_t)n < sizeof(msg) ? (size_t)n : sizeof(msg) - 1);
}

// Close the file (once); a failure to write it out is reported unless one was already.
static void tee_finish(tee_t *t, bool report) {
    if (!t->file)
        return;
    const char *err = pipe_file_close(t->file);
    t->file = NULL;
    if (err && report)
        tee_say(t, err);
}

static void tee_write(pipe_stage_t *st, const char *buf, size_t len) {
    tee_t *t = (tee_t *)st;
    if (t->file) {
        const char *err = pipe_file_write(t->file, buf, len);
        if (err) {                          // once: the file is closed, the line ends
            tee_finish(t, false);
            pipe_emit(st, buf, len);        // what the terminal was owed from this write
            tee_say(t, err);
            pipe_shut(st);
            return;
        }
    }
    pipe_emit(st, buf, len);
}

static void tee_end(pipe_stage_t *st) {
    tee_finish((tee_t *)st, true);
}

static void tee_close(pipe_stage_t *st) {
    tee_finish((tee_t *)st, false);         // cancelled: a partial file stays, as in Unix
    stage_free(st);
}

static pipe_stage_t *tee_open(shell_t *sh, const char *args) {
    bool append = false;
    const char *a = args ? args : "";
    if (a[0] == '-' && a[1] == 'a' && (a[2] == ' ' || !a[2])) {
        append = true;
        a += 2;
        while (*a == ' ')
            a++;
    }
    if (!*a || strchr(a, ' ') || strlen(a) > SHELL_PATH_MAX) {
        shell_printf(sh, "usage: <command> | tee [-a] <path>\n");
        return NULL;
    }
    tee_t *t = stage_new(sh, &tee_filter, sizeof(tee_t));
    if (!t)
        return NULL;
    t->file = pipe_file_open(sh, a, append);
    if (!t->file) {                         // why was printed
        stage_free(&t->st);
        return NULL;
    }
    strcpy(t->path, a);
    return &t->st;
}

static const pipe_filter_t tee_filter = {"tee", tee_open, tee_write, tee_end, tee_close};

// --- parsing ---------------------------------------------------------------------------------

static const pipe_filter_t *const filters[] = {&grep_filter, &head_filter, &tail_filter,
                                               &wc_filter, &tee_filter, NULL};

// The sink `more` is a stage name too, though not a filter.
static const char *const sink_more = "more";

// Does `name` start `word`, the word ending there (at a space, a '|' or the end)?
static bool word_is(const char *word, const char *name) {
    size_t n = strlen(name);
    return !strncmp(word, name, n) && (word[n] == '\0' || word[n] == ' ' || word[n] == '|');
}

bool pipe_is_stage(const char *word) {
    if (word_is(word, sink_more))
        return true;
    for (const pipe_filter_t *const *f = filters; *f; f++)
        if (word_is(word, (*f)->name))
            return true;
    return false;
}

// The first '|' in `s` that starts a stage, or NULL.
static char *next_stage(char *s) {
    for (char *bar = strchr(s, '|'); bar; bar = strchr(bar + 1, '|')) {
        const char *w = bar + 1;
        while (*w == ' ')
            w++;
        if (pipe_is_stage(w))
            return bar;
    }
    return NULL;
}

char *pipe_split(char *line) {
    char *bar = next_stage(line);
    if (!bar)
        return NULL;
    char *end = bar;
    while (end > line && end[-1] == ' ')
        end--;
    *bar = '\0';
    *end = '\0';
    return bar + 1;
}

// Trim the spaces around s in place.
static char *trim(char *s) {
    while (*s == ' ')
        s++;
    char *end = s + strlen(s);
    while (end > s && end[-1] == ' ')
        *--end = '\0';
    return s;
}

bool pipe_open(shell_t *sh, pipeline_t *p, char *stages) {
    p->first = NULL;
    p->done = false;
    p->sink = PIPE_SINK_TERMINAL;
    p->sh = sh;
    // The chain is built apart and attached at the end: until then nothing is piped, so a later
    // stage's usage message goes to the terminal and not through the stages before it.
    pipe_stage_t *first = NULL;
    pipe_stage_t **link = &first;
    int count = 0;
    for (char *s = stages; s;) {
        char *bar = next_stage(s);
        if (bar)
            *bar = '\0';
        char *name = trim(s);
        s = bar ? bar + 1 : NULL;
        char *args = strchr(name, ' ');
        if (args) {
            *args = '\0';
            args = trim(args + 1);
        }
        if (args && !*args)
            args = NULL;
        if (!strcmp(name, sink_more)) {
            if (s || args) {
                shell_printf(sh, s ? "more: only at the end of the line\n"
                                   : "usage: <command> | more\n");
                goto fail;
            }
            p->sink = PIPE_SINK_PAGER;
            break;
        }
        const pipe_filter_t *f = NULL;
        for (const pipe_filter_t *const *x = filters; *x && !f; x++)
            if (!strcmp(name, (*x)->name))
                f = *x;
        if (!f)                             // can't happen: next_stage() found a stage name
            goto fail;
        if (++count > PIPE_STAGES_MAX) {
            shell_printf(sh, "%s: %d filters in a line at most\n", name, PIPE_STAGES_MAX);
            goto fail;
        }
        pipe_stage_t *st = f->open(sh, args);
        if (!st)                            // why was printed
            goto fail;
        st->pipe = p;
        *link = st;
        link = &st->next;
    }
    p->first = first;
    return true;
fail:
    p->first = first;
    pipe_close(p);
    p->sink = PIPE_SINK_TERMINAL;
    return false;
}

// --- Tab -------------------------------------------------------------------------------------

static const shell_word_t grep_options[4];
static const shell_word_t grep_options[4] = {
    {"-i", grep_options, 0}, {"-n", grep_options, 0}, {"-v", grep_options, 0}, {0}};
static const shell_word_t tee_options[2] = {{"-a", NULL, SHELL_PATH}, {0}};
const shell_cmd_t pipe_stage_table[] = {
    {"grep", NULL, grep_options, 0},
    {"head", NULL, NULL, 0},
    {"more", NULL, NULL, 0},
    {"tail", NULL, NULL, 0},
    {"tee", NULL, tee_options, SHELL_PATH},
    {"wc", NULL, NULL, 0},
    {0}};
