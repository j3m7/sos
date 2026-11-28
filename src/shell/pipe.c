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

// A count of lines for head and tail: 10 without one, else 1 to 999999. One word at most.
static bool parse_lines(int argc, char **argv, unsigned long *n) {
    if (argc > 2)
        return false;
    if (argc < 2) {
        *n = 10;
        return true;
    }
    const char *args = argv[1];
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

static pipe_stage_t *grep_open(shell_t *sh, int argc, char **argv) {
    bool invert = false, nocase = false, number = false;
    // Words of option letters come first, then one word, the text, as in sh: 'grep "no route"'
    // (a word that starts with '-' but isn't all option letters, "-x" or "-", is the text).
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        size_t n = 1;
        while (argv[i][n] == 'v' || argv[i][n] == 'i' || argv[i][n] == 'n')
            n++;
        if (argv[i][n])
            break;
        invert |= strchr(argv[i], 'v') != NULL;
        nocase |= strchr(argv[i], 'i') != NULL;
        number |= strchr(argv[i], 'n') != NULL;
    }
    if (i != argc - 1 || !*argv[i]) {
        shell_printf(sh, "usage: <command> | grep [-v] [-i] [-n] <text>\n");
        return NULL;
    }
    const char *text = argv[i];
    size_t plen = strlen(text);
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
    for (size_t j = 0; j < plen; j++)
        g->pattern[j] = nocase ? fold(text[j]) : text[j];
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

static pipe_stage_t *head_open(shell_t *sh, int argc, char **argv) {
    unsigned long n;
    if (!parse_lines(argc, argv, &n)) {
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

static pipe_stage_t *tail_open(shell_t *sh, int argc, char **argv) {
    unsigned long n;
    if (!parse_lines(argc, argv, &n)) {
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

static pipe_stage_t *wc_open(shell_t *sh, int argc, char **argv) {
    (void)argv;
    if (argc > 1) {
        shell_printf(sh, "usage: <command> | wc\n");
        return NULL;
    }
    wc_t *w = stage_new(sh, &wc_filter, sizeof(wc_t));
    return w ? &w->st : NULL;
}

static const pipe_filter_t wc_filter = {"wc", wc_open, wc_write, wc_end, stage_free};

// --- tee [-a] <path> ------------------------------------------------------------------------

// The card side, unless files.c (or a test) supplies it: no card, nothing opens.
__attribute__((weak)) bool pipe_file_check(shell_t *sh, const char *name, char *path, size_t size) {
    (void)path;
    (void)size;
    shell_printf(sh, "tee: %s: no SD card\n", name);
    return false;
}

__attribute__((weak)) pipe_file_t *pipe_file_open(const char *path, bool append, const char **why) {
    (void)path;
    (void)append;
    *why = "no SD card";
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
    pipe_file_t *file;          // the file, from the first output on; NULL before, and once closed
    bool append;
    bool quiet;                 // passes nothing on: the shell's `> path` (the line's last stage)
    bool done;                  // closed (the output ended, or a write failed): don't open again
    char path[SHELL_PATH_MAX + 1];  // as typed, for the messages
    char abs[PIPE_FILE_PATH];   // resolved and checked when the line was parsed
} tee_t;

static const pipe_filter_t tee_filter;

// Say why, straight to the sink (the terminal, or the pager's text under `| more`): not down
// the line, where a later filter could drop it, and not into the file, which is closed by now.
static void tee_say(tee_t *t, const char *what) {
    char msg[SHELL_PATH_MAX + 80];
    int n = snprintf(msg, sizeof(msg), "tee: %s: %s\n", t->path, what);
    if (n > 0)
        t->st.pipe->out(t->st.pipe->sh, msg, (size_t)n < sizeof(msg) ? (size_t)n : sizeof(msg) - 1);
}

// Close the file (once); a failure to write it out is reported unless one was already.
static void tee_finish(tee_t *t, bool report) {
    t->done = true;
    if (!t->file)
        return;
    const char *err = pipe_file_close(t->file);
    t->file = NULL;
    if (err && report)
        tee_say(t, err);
}

// The file is opened on the first output (or at the end, for an empty output): a line that
// doesn't parse, or a command that refuses to run, leaves it as it was.
static const char *tee_put(tee_t *t, const char *buf, size_t len) {
    if (t->done)
        return NULL;
    if (!t->file) {
        const char *why;
        t->file = pipe_file_open(t->abs, t->append, &why);
        if (!t->file)
            return why;
    }
    return len ? pipe_file_write(t->file, buf, len) : NULL;
}

static void tee_write(pipe_stage_t *st, const char *buf, size_t len) {
    tee_t *t = (tee_t *)st;
    const char *err = tee_put(t, buf, len);
    if (!t->quiet)
        pipe_emit(st, buf, len);            // what the sink was owed from this write
    if (err) {                              // once: the file is closed, the line ends
        tee_finish(t, false);
        tee_say(t, err);
        pipe_shut(st);
    }
}

static void tee_end(pipe_stage_t *st) {
    tee_t *t = (tee_t *)st;
    const char *err = NULL;
    if (!t->done && !t->file)               // no output at all: the file still gets made, empty
        err = tee_put(t, "", 0);
    tee_finish(t, true);
    if (err)
        tee_say(t, err);
}

static void tee_close(pipe_stage_t *st) {
    tee_finish((tee_t *)st, false);         // cancelled: a partial file stays, as in Unix
    stage_free(st);
}

// A tee stage for `path`, checked now (a refusal is said before the command runs); NULL after
// printing why.
static pipe_stage_t *tee_new(shell_t *sh, const char *path, bool append, bool quiet) {
    tee_t *t = stage_new(sh, &tee_filter, sizeof(tee_t));
    if (!t)
        return NULL;
    if (!pipe_file_check(sh, path, t->abs, sizeof(t->abs))) {     // why was printed
        stage_free(&t->st);
        return NULL;
    }
    t->append = append;
    t->quiet = quiet;
    strcpy(t->path, path);
    return &t->st;
}

static pipe_stage_t *tee_open(shell_t *sh, int argc, char **argv) {
    bool append = argc > 1 && !strcmp(argv[1], "-a");
    int i = append ? 2 : 1;
    if (argc != i + 1 || !*argv[i] || strlen(argv[i]) > SHELL_PATH_MAX) {
        shell_printf(sh, "usage: <command> | tee [-a] <path>\n");
        return NULL;
    }
    return tee_new(sh, argv[i], append, false);
}

static const pipe_filter_t tee_filter = {"tee", tee_open, tee_write, tee_end, tee_close};

// --- parsing ---------------------------------------------------------------------------------

static const pipe_filter_t *const filters[] = {&grep_filter, &head_filter, &tail_filter,
                                               &wc_filter, &tee_filter, NULL};

// The sink `more` is a stage name too, though not a filter.
static const char *const sink_more = "more";

static const pipe_filter_t *find_filter(const char *name) {
    for (const pipe_filter_t *const *f = filters; *f; f++)
        if (!strcmp(name, (*f)->name))
            return *f;
    return NULL;
}

// The plain words from w->v[i] on, up to the next operator or the end.
static int count_plain(const words_t *w, int i) {
    int n = 0;
    while (i + n < w->n && w->v[i + n])
        n++;
    return n;
}

static const char *redirect_name(const words_t *w, int i) {
    return w->kind[i] == WORD_APPEND ? ">>" : ">";
}

// What follows the command, against the grammar (docs/design/shell-quoting.md §3), before any
// stage opens: false after saying why.
static bool check_stages(shell_t *sh, const words_t *w, int i) {
    while (i < w->n) {
        if (w->kind[i] == WORD_PIPE) {
            const char *name = w->v[i + 1];     // w->v[w->n] is NULL
            if (!name) {
                shell_printf(sh, "|: a filter must follow (grep, head, tail, wc, tee, more)\n");
                return false;
            }
            bool more = !strcmp(name, sink_more);
#if !SOS_HAVE_PAGER
            if (more) {
                shell_printf(sh, "more: not in this build (no pager)\n");
                return false;
            }
#endif
            if (!more && !find_filter(name)) {
                shell_printf(sh, "%s: not a filter (grep, head, tail, wc, tee, more)\n", name);
                return false;
            }
            i += 1 + count_plain(w, i + 1);
            if (more && i < w->n) {
                shell_printf(sh, "more: only at the end of the line\n");
                return false;
            }
            continue;
        }
        const char *op = redirect_name(w, i);   // > or >>: one path, then the end
        if (!w->v[i + 1]) {
            shell_printf(sh, "usage: <command> %s <path>\n", op);
            return false;
        }
        if (i + 2 < w->n) {
            if (w->v[i + 2])
                shell_printf(sh, "%s: one path (quote a name that has spaces)\n", op);
            else if (w->kind[i + 2] == WORD_PIPE)
                shell_printf(sh, "%s: only at the end of the line\n", op);
            else
                shell_printf(sh, "%s: one in a line at most\n", op);
            return false;
        }
        i += 2;
    }
    return true;
}

bool pipe_open(shell_t *sh, pipeline_t *p, words_t *w, int start) {
    p->first = NULL;
    p->done = false;
    p->sink = PIPE_SINK_TERMINAL;
    p->sh = sh;
    if (!check_stages(sh, w, start))
        return false;
    // The chain is built apart and attached at the end: until then nothing is piped, so a later
    // stage's usage message goes to the terminal and not through the stages before it.
    pipe_stage_t *first = NULL;
    pipe_stage_t **link = &first;
    int count = 0;
    for (int i = start; i < w->n;) {
        pipe_stage_t *st;
        if (w->kind[i] != WORD_PIPE) {      // `> path`: a quiet tee, the last stage
            st = tee_new(sh, w->v[i + 1], w->kind[i] == WORD_APPEND, true);
            if (!st)
                goto fail;
            st->pipe = p;
            *link = st;
            break;
        }
        char **argv = &w->v[i + 1];         // argv[argc] is NULL: an operator, or the end
        int argc = count_plain(w, i + 1);
        i += 1 + argc;
        if (!strcmp(argv[0], sink_more)) {
            if (argc > 1) {
                shell_printf(sh, "usage: <command> | more\n");
                goto fail;
            }
            p->sink = PIPE_SINK_PAGER;
            break;
        }
        if (++count > PIPE_STAGES_MAX) {
            shell_printf(sh, "%s: %d filters in a line at most\n", argv[0], PIPE_STAGES_MAX);
            goto fail;
        }
        const pipe_filter_t *flt = find_filter(argv[0]);
        if (!flt)                           // can't happen: check_stages() found it
            goto fail;
        st = flt->open(sh, argc, argv);
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
    {"grep", NULL, NULL, grep_options, 0},
    {"head", NULL, NULL, NULL, 0},
#if SOS_HAVE_PAGER
    {"more", NULL, NULL, NULL, 0},
#endif
    {"tail", NULL, NULL, NULL, 0},
    {"tee", NULL, NULL, tee_options, SHELL_PATH},
    {"wc", NULL, NULL, NULL, 0},
    {0}};
