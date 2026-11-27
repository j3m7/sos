// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Shell line editing and output. Depends only on shell.h, sos_version, login_gate() and the C
// library, so it builds on the host for unit tests (which define sos_version, and login_gate()
// unless they link login.c); the commands live in shell.c (shell_execute).

#include "shell/shell.h"
#include "core/version.h"
#include "shell/charwidth.h"
#include "shell/complete.h"
#include "shell/login.h"
#include "shell/termtext.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/heaptag.h"

// --- pipelines: `<command> | grep x | more` (pipe.h) --------------------------------------------

#define PIPE_STEP 2048                  // the capture grows by this much

static void term_out(shell_t *sh, const char *buf, size_t len);

// Is a line's output going through a pipeline now?
static bool piped(const shell_t *sh) {
    return sh->pipe.first || sh->pipe.sink == PIPE_SINK_PAGER;
}

// The sink `more`: keep the output instead of sending it, up to SHELL_PIPE_MAX bytes. Once full
// the producer has done its work: it is ended as head ends it.
static void pipe_append(shell_t *sh, const char *buf, size_t len) {
    pipeline_t *p = &sh->pipe;
    if (p->cut)
        return;
    if (p->len + len > SHELL_PIPE_MAX) {
        len = SHELL_PIPE_MAX - p->len;
        p->cut = p->done = true;
    }
    if (p->len + len > p->cap) {
        size_t cap = p->cap;
        while (cap < p->len + len)
            cap += PIPE_STEP;
        char *b = heap_realloc(HEAP_EDIT, p->buf, cap);
        if (!b) {                       // the rest is lost: say so in the text
            p->cut = p->oom = p->done = true;
            return;
        }
        p->buf = b;
        p->cap = cap;
    }
    memcpy(p->buf + p->len, buf, len);
    p->len += len;
}

// Back to idle, the filters already closed; the capture's block is the caller's.
static void pipe_reset(pipeline_t *p) {
    p->buf = NULL;
    p->len = p->cap = 0;
    p->cut = p->oom = p->pending = p->done = p->aside = false;
    p->paging = p->capturing = p->released = p->page_later = false;
    p->prod = NULL;
    p->prod_arg = NULL;
    p->sink = PIPE_SINK_TERMINAL;
}

// The line is cancelled (Ctrl-C, the session ended): the filters' held text and the capture go.
static void pipe_drop(shell_t *sh) {
    pipe_close(&sh->pipe);
    heap_release(HEAP_EDIT, sh->pipe.buf);
    pipe_reset(&sh->pipe);
}

// The command turned out not to be one to pipe (it asks a question, or draws its own screen):
// the filters are closed, what was captured so far goes to the screen, and the rest will too.
static void pipe_release(shell_t *sh) {
    pipe_close(&sh->pipe);
    char *text = sh->pipe.buf;
    size_t len = sh->pipe.len;
    pipe_reset(&sh->pipe);
    if (len)
        term_out(sh, text, len);
    heap_release(HEAP_EDIT, text);
}

// The command is done: the filters flush what they hold, and the line is idle again. With `more`
// the capture is returned, a HEAP_EDIT block that is the caller's (with the note of a cut
// appended, unless it is empty); with the terminal sink, NULL.
static char *pipe_complete(shell_t *sh, size_t *len) {
    pipeline_t *p = &sh->pipe;
    pipe_end(p);
    char *text = p->buf;
    bool pager = p->sink == PIPE_SINK_PAGER;
    bool cut = p->cut, oom = p->oom;
    *len = p->len;
    pipe_reset(p);
    if (!pager)
        return NULL;
    if (cut && *len) {
        const char *note = oom ? "\n[out of memory: the output is cut here]\n"
                               : "\n[output cut here: the pager takes 32 KB at most]\n";
        size_t nl = strlen(note);
        char *b = heap_realloc(HEAP_EDIT, text, *len + nl);
        if (b) {
            text = b;
            memcpy(text + *len, note, nl);
            *len += nl;
        }
    }
    return text;
}

// The command is done: with `more` the capture goes to the pager (which owns the block from here
// on).
static void pipe_finish(shell_t *sh) {
    bool pager = sh->pipe.sink == PIPE_SINK_PAGER;
    size_t len;
    char *text = pipe_complete(sh, &len);
    if (!pager)
        return;
    if (len == 0) {
        heap_release(HEAP_EDIT, text);
        shell_printf(sh, "%s: no output\n", sh->pipe.name);
        return;
    }
    shell_page(sh, sh->pipe.name, text, len);
}

// A stage closed the stream (head) or the capture is full: end the producing job now, silently
// (no ^C), as if it had finished.
static void end_job_quietly(shell_t *sh) {
    shell_job_fn step = sh->job;
    void *arg = sh->job_arg;
    sh->job = NULL;
    sh->job_arg = NULL;
    sh->key = NULL;
    step(sh, arg, true);
}

// --- output --------------------------------------------------------------------

// The terminal: the channel, with its newline translation.
static void term_out(shell_t *sh, const char *buf, size_t len) {
    if (len)
        sh->at_line_start = buf[len - 1] == '\n';
    if (!sh->io->crlf) {
        sh->io->write(sh->ctx, buf, (int)len);
        return;
    }
    // Translate "\n" to "\r\n" through a buffer, so the channel gets a few large writes (on
    // SSH, each write can become an encrypted packet) rather than one per line and per "\r\n".
    char out[256];
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (n + 2 > sizeof(out)) {
            sh->io->write(sh->ctx, out, (int)n);
            n = 0;
        }
        if (buf[i] == '\n')
            out[n++] = '\r';
        out[n++] = buf[i];
    }
    if (n)
        sh->io->write(sh->ctx, out, (int)n);
}

// Does what the shell prints now go into the line's pipeline? Not output from outside the line
// (aside), and not the pager's own while it shows a producer that still runs (paging): only the
// producer's steps (capturing) go to the capture then.
static bool to_pipe(const shell_t *sh) {
    const pipeline_t *p = &sh->pipe;
    return piped(sh) && !p->aside && (!p->paging || p->capturing);
}

// Everything the shell and its commands print: through the line's pipeline when there is one
// (nothing on screen with `| more`), else to the terminal.
static void write_out(shell_t *sh, const char *buf, size_t len) {
    if (to_pipe(sh))
        pipe_write(&sh->pipe, buf, len);
    else
        term_out(sh, buf, len);
}

void shell_write(shell_t *sh, const char *buf, size_t len) {
    write_out(sh, buf, len);
}

void shell_printf(shell_t *sh, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(buf))
        n = sizeof(buf) - 1;    // truncated; callers keep individual lines short
    write_out(sh, buf, (size_t)n);
}

static void prompt(shell_t *sh) {
    shell_printf(sh, "%s", sh->answer ? sh->ask_prompt : SHELL_PROMPT);
}

void shell_ask(shell_t *sh, const char *ask_prompt, bool secret, shell_answer_fn answer, void *arg) {
    sh->ask_prompt = ask_prompt;
    sh->secret = secret;
    sh->ask_eof = false;
    sh->ask_complete = NULL;
    sh->answer = answer;
    sh->answer_arg = arg;
}

// The three ways a question's answer is completed (shell.h); never for a secret question.
static void ask_complete(shell_t *sh, shell_ask_complete_fn complete, bool words,
                         const char *chars) {
    if (sh->answer && !sh->secret) {
        sh->ask_complete = complete;
        sh->ask_words = words;
        sh->ask_chars = chars;
    }
}

void shell_ask_complete(shell_t *sh, shell_ask_complete_fn complete) {
    ask_complete(sh, complete, false, NULL);
}

void shell_ask_complete_words(shell_t *sh, shell_ask_complete_fn complete) {
    ask_complete(sh, complete, true, NULL);
}

void shell_ask_complete_chars(shell_t *sh, shell_ask_complete_fn complete, const char *chars) {
    ask_complete(sh, complete, true, chars);
}

char shell_eof[] = "";

void shell_ask_eof(shell_t *sh, const char *ask_prompt, shell_answer_fn answer, void *arg) {
    shell_ask(sh, ask_prompt, false, answer, arg);
    sh->ask_eof = true;
}

static void wipe(void *p, size_t n) {
    volatile char *v = p;
    for (size_t i = 0; i < n; i++)
        v[i] = 0;
}

void shell_clear_history(shell_t *sh) {
    wipe(sh->history, sizeof(sh->history));
    wipe(sh->draft, sizeof(sh->draft));
    sh->history_len = 0;
    sh->history_pos = -1;
    sh->draft_len = 0;
}

void shell_discard_input(shell_t *sh) {
    wipe(sh->line, sizeof(sh->line));
    wipe(sh->ahead, sizeof(sh->ahead));
    shell_clear_history(sh);
    sh->line_len = 0;
    sh->ahead_len = 0;
    sh->last_was_cr = false;
    sh->keys = (keys_t)KEYS_INIT;
    sh->tab_pressed = false;
}

void shell_cancel_question(shell_t *sh) {
    if (sh->secret) {                   // a half-typed secret must not become a command
        wipe(sh->line, sizeof(sh->line));
        sh->line_len = 0;
    }
    sh->answer = NULL;
    sh->secret = false;
    sh->ask_eof = false;
    sh->ask_complete = NULL;
}

// Take the pending question, if any, and hand it the answer (NULL = cancelled).
static bool answer_question(shell_t *sh, char *answer) {
    shell_answer_fn fn = sh->answer;
    if (!fn)
        return false;
    void *arg = sh->answer_arg;
    sh->answer = NULL;                  // cleared first: the callback may ask the next question
    sh->secret = false;
    sh->ask_eof = false;
    sh->ask_complete = NULL;
    fn(sh, answer, arg);
    return true;
}

void shell_hide_input(shell_t *sh) {
    if (!sh->io || shell_screen_held(sh))       // full-screen program, or `| more`: nothing shown
        return;
    sh->pipe.aside = piped(sh);                 // what follows isn't the command's: no filters
    if (sh->job) {                              // job output on screen: don't erase it
        if (!sh->at_line_start)
            write_out(sh, "\n", 1);
        return;
    }
    sh->io->write(sh->ctx, "\r\x1b[K", 4);    // to column 0, erase the line
}

void shell_show_input(shell_t *sh) {
    if (sh->io)
        sh->pipe.aside = false;                 // the command's output is filtered again
    if (!sh->io || sh->job)                     // no prompt while a job runs
        return;
    prompt(sh);
    if (!sh->secret && sh->line_len)
        sh->io->write(sh->ctx, sh->line, (int)sh->line_len);
}

void shell_logout(shell_t *sh) {
    if (sh->io->logout)
        sh->io->logout(sh->ctx);
    else
        shell_printf(sh, "logout: the %s session can't be ended\n", sh->io->name);
}

// --- command history (Up/Down) ----------------------------------------------------------------

// Remember a command, unless it repeats the newest one. The oldest entries make room.
static void history_add(shell_t *sh, const char *cmd) {
    size_t n = strlen(cmd) + 1;
    if (n > sizeof(sh->history))
        return;                         // can't happen: a line is shorter than the history
    if (sh->history_len) {              // the newest entry starts after the '\0' before its own
        size_t start = sh->history_len - 1;
        while (start > 0 && sh->history[start - 1] != '\0')
            start--;
        if (!strcmp(sh->history + start, cmd))
            return;
    }
    size_t drop = 0;
    while (sh->history_len - drop + n > sizeof(sh->history))
        drop += strlen(sh->history + drop) + 1;
    if (drop) {
        memmove(sh->history, sh->history + drop, sh->history_len - drop);
        sh->history_len -= drop;
        wipe(sh->history + sh->history_len, drop);
    }
    memcpy(sh->history + sh->history_len, cmd, n);
    sh->history_len += n;
}

static int history_count(const shell_t *sh) {
    int count = 0;
    for (size_t i = 0; i < sh->history_len; i++)
        count += sh->history[i] == '\0';
    return count;
}

// The entry `back` places before the newest (0 = the newest).
static const char *history_entry(const shell_t *sh, int back) {
    int index = history_count(sh) - 1 - back;
    const char *p = sh->history;
    while (index-- > 0)
        p += strlen(p) + 1;
    return p;
}

// The line is being changed by hand (or entered): a recalled line becomes ordinary input, and
// the next Up starts again from the newest command.
static void history_edited(shell_t *sh) {
    if (sh->history_pos < 0)
        return;
    sh->history_pos = -1;
    wipe(sh->draft, sh->draft_len);
    sh->draft_len = 0;
}

// Put text in place of the line being typed, and redraw it.
static void line_replace(shell_t *sh, const char *text, size_t len) {
    shell_hide_input(sh);
    memcpy(sh->line, text, len);
    sh->line_len = len;
    shell_show_input(sh);
}

// Up: the previous command (the line being typed is kept for Down). Down: the next one, and
// after the newest, the line that was being typed.
static void history_key(shell_t *sh, int key) {
    if (sh->answer)                     // a question: its answers aren't commands
        return;
    if (key == KEY_UP) {
        if (sh->history_pos + 1 >= history_count(sh))
            return;
        if (sh->history_pos < 0) {
            memcpy(sh->draft, sh->line, sh->line_len);
            sh->draft_len = sh->line_len;
        }
        sh->history_pos++;
        const char *e = history_entry(sh, sh->history_pos);
        line_replace(sh, e, strlen(e));
    } else if (key == KEY_DOWN) {
        if (sh->history_pos < 0)
            return;
        sh->history_pos--;
        if (sh->history_pos < 0) {
            line_replace(sh, sh->draft, sh->draft_len);
            wipe(sh->draft, sh->draft_len);
            sh->draft_len = 0;
        } else {
            const char *e = history_entry(sh, sh->history_pos);
            line_replace(sh, e, strlen(e));
        }
    }
}

// --- `| more` while the producer runs ---------------------------------------------------------

// Should the pager take over a `| more` line now? Its producer is a job (not a full-screen
// program, nothing asked) that still runs, and it has given the first output.
static bool page_now(const shell_t *sh) {
    const pipeline_t *p = &sh->pipe;
    return p->pending && p->sink == PIPE_SINK_PAGER && !p->paging && !p->page_later &&
           !p->done && p->len > 0 && sh->job && !sh->key && !sh->answer;
}

// Hand the screen to the pager, which steps the producer itself from now on (shell_pipe_feed):
// the producer's job is kept in the pipeline and the pager's becomes the shell's. Input typed
// meanwhile was meant for the shell, not keys for the pager: it goes. If the pager can't start,
// the line is paged when the producer ends, as before.
static void page_early(shell_t *sh) {
    pipeline_t *p = &sh->pipe;
    p->prod = sh->job;
    p->prod_arg = sh->job_arg;
    sh->job = NULL;
    sh->job_arg = NULL;
    p->paging = true;                       // from here the pager's output reaches the screen
    wipe(sh->ahead, sh->ahead_len);
    sh->ahead_len = 0;
    if (shell_page_growing(sh, p->name, p->buf, p->len))
        return;
    p->paging = false;                      // it said why; the producer runs on, captured
    sh->job = p->prod;
    sh->job_arg = p->prod_arg;
    p->prod = NULL;
    p->prod_arg = NULL;
    p->page_later = true;
}

// Invariant for job authors: a producer's step must not end the session (shell_end) or start a
// job of its own (shell_run_job, shell_run_raw) while it is paged; it may ask a question. The
// session's end re-enters the step through the pager's cancel, and a new job would replace the
// pager's with the terminal left in the alternate screen. No producer does either today.
shell_fed_t shell_pipe_feed(shell_t *sh, shell_feed_t how, char **text, size_t *len) {
    pipeline_t *p = &sh->pipe;
    *text = NULL;
    *len = 0;
    if (!p->paging || !p->prod)             // can't happen: no producer under a pager
        return SHELL_FED_GONE;
    shell_job_fn step = p->prod;
    void *arg = p->prod_arg;
    bool more = false;
    p->capturing = true;
    if (how == SHELL_FEED_STEP) {
        more = step(sh, arg, false);
        if (more && p->done && !sh->answer) {   // head has its lines, or the capture is full:
            step(sh, arg, true);                // ended silently
            more = false;
        }
    } else {
        step(sh, arg, true);
    }
    p->capturing = false;
    if (how == SHELL_FEED_CANCEL) {
        pipe_drop(sh);
        return SHELL_FED_GONE;
    }
    if (sh->answer) {                       // it asks: not a thing to page; the shell shows the
        p->prod = more ? step : NULL;       // text and the question once the pager has left,
        p->prod_arg = more ? arg : NULL;    // and resumes the producer if it still runs
        p->released = true;
        return SHELL_FED_GONE;
    }
    if (more) {
        *text = p->buf;
        *len = p->len;
        return SHELL_FED_MORE;
    }
    *text = pipe_complete(sh, len);
    return SHELL_FED_DONE;
}

// The pager has left because its producer asked a question: the text so far goes to the
// screen, and a producer that still runs is the shell's job again (the question is asked when it
// ends, as for any job that asks).
static void pipe_resume(shell_t *sh) {
    shell_job_fn prod = sh->pipe.prod;
    void *arg = sh->pipe.prod_arg;
    pipe_release(sh);
    if (prod)
        shell_run_job(sh, prod, arg);
}

// Trim spaces, split the line into words (words.h) and hand the command to the command module.
// A line with stages (`<command> | grep x | more`) or a redirect (`<command> > file`) runs the
// command with its output going through them: to the terminal, into the file, or captured and
// then paged with `more` last.
static void run_line(shell_t *sh, char *cmd) {
    while (*cmd == ' ')
        cmd++;
    char *end = cmd + strlen(cmd);
    while (end > cmd && end[-1] == ' ')
        *--end = '\0';
    if (!*cmd)
        return;
    history_add(sh, cmd);
    pipeline_t *p = &sh->pipe;
    snprintf(p->name, sizeof(p->name), "%s", cmd);
    // The console takes no command while it's locked (shell/login.h). The gate sees the line
    // as typed, before it is split: a locked console gets its refusal (or runs a rescue
    // command, which login.c knows by its whole line), never a message about quotes.
    if (sh->io->admin && login_gate(sh, cmd))
        return;
    words_t w;
    const char *why = words_split(cmd, &w);
    int argc = 0;                           // the command: the words before the first operator
    while (!why && argc < w.n && w.v[argc])
        argc++;
    if (why) {                              // nothing runs
        shell_printf(sh, "%s\n", why);
        return;
    }
    if (!argc) {
        shell_printf(sh, "usage: <command> | more, or <command> | grep|head|tail|wc|tee ..., "
                         "or <command> > <path>\n");
        return;
    }
    if (argc == w.n) {                      // no stages
        shell_execute(sh, argc, w.v);
        return;
    }
    if (!pipe_open(sh, p, &w, argc))        // a stage doesn't parse: why, and nothing runs
        return;
    p->out = term_out;
    if (p->sink == PIPE_SINK_PAGER) {
        p->buf = heap_alloc(HEAP_EDIT, PIPE_STEP);
        if (p->buf) {
            p->cap = PIPE_STEP;
            p->out = pipe_append;
        } else {
            p->sink = PIPE_SINK_TERMINAL;
            shell_printf(sh, "%s: out of memory, running without the pager\n", p->name);
        }
    }
    shell_execute(sh, argc, w.v);
    if (!piped(sh))                         // nothing to pipe through (the pager was out of
        return;                             // memory), or the command ended the session
    if (sh->answer)                         // it asks a question: not a thing to pipe
        pipe_release(sh);
    else if (sh->job && sh->key)            // a full-screen program: its own screen; what it
        pipe_release(sh);                   // printed before (a warning) shows under it
    else if (sh->job && !p->done) {         // a job: the line ends when it does, and with
        p->pending = true;                  // `more` the pager starts on its first output
        if (page_now(sh))
            page_early(sh);
    } else {
        if (sh->job)                        // it has given all that is wanted (head)
            end_job_quietly(sh);
        pipe_finish(sh);
    }
}

// --- input ---------------------------------------------------------------------

void shell_bind(shell_t *sh, const shell_io_t *io, void *ctx) {
    sh->io = io;
    sh->ctx = ctx;
    sh->line_len = 0;
    sh->last_was_cr = false;
    sh->keys = (keys_t)KEYS_INIT;
    sh->tab_pressed = false;
    sh->logger_quota = (log_quota_t){0};    // a reused SSH slot starts with its own count
    sh->answer = NULL;
    sh->secret = false;
    sh->ask_eof = false;
    sh->ask_complete = NULL;
    sh->job = NULL;             // a job of an earlier session was cancelled by shell_end()
    sh->job_arg = NULL;
    sh->key = NULL;
    pipe_drop(sh);
    sh->ahead_len = 0;
    strcpy(sh->cwd, "/");
    shell_clear_history(sh);    // a new session doesn't see the last one's commands
}

void shell_greet(shell_t *sh) {
    shell_printf(sh, "\nSOS %s\nType 'help' for commands.\n", sos_version);
}

void shell_init(shell_t *sh, const shell_io_t *io, void *ctx) {
    shell_bind(sh, io, ctx);
    shell_greet(sh);
    prompt(sh);
}

void shell_run_job(shell_t *sh, shell_job_fn step, void *arg) {
    sh->job = step;
    sh->job_arg = arg;
    sh->key = NULL;
}

void shell_run_raw(shell_t *sh, shell_job_fn step, shell_key_fn key, void *arg) {
    shell_run_job(sh, step, arg);
    sh->key = key;
}

bool shell_job_running(const shell_t *sh) {
    return sh->job != NULL;
}

bool shell_raw_mode(const shell_t *sh) {
    return sh->job != NULL && sh->key != NULL;
}

bool shell_output_piped(const shell_t *sh) {
    return to_pipe(sh);
}

bool shell_screen_held(const shell_t *sh) {
    return shell_raw_mode(sh) || sh->pipe.sink == PIPE_SINK_PAGER;
}

bool shell_busy(const shell_t *sh) {
    return sh->job != NULL || sh->ahead_len > 0;
}

int shell_write_space(shell_t *sh) {
    // A capture waits for no channel; the pager over a running producer writes to the channel
    // between the producer's steps, and waits for it.
    if ((sh->pipe.sink == PIPE_SINK_PAGER && to_pipe(sh)) || !sh->io->write_space)
        return 1024;
    int n = sh->io->write_space(sh->ctx);
    return n > 0 ? n : 0;
}

void shell_end(shell_t *sh) {
    if (sh->job) {
        shell_job_fn step = sh->job;
        sh->job = NULL;
        sh->key = NULL;
        step(sh, sh->job_arg, true);
    }
    pipe_drop(sh);
    sh->answer = NULL;
    sh->ask_complete = NULL;
    shell_clear_history(sh);    // the session's commands don't wait in RAM for the next one
}

// While a job runs: only Ctrl-C counts; then one step.
// Keep input that arrives while a job runs, to process once it's done. Beyond the buffer, input
// stays unread in the channel (an SSH client then waits).
static void stash(shell_t *sh, const char *buf, size_t n) {
    size_t room = sizeof(sh->ahead) - sh->ahead_len;
    if (n > room)
        n = room;               // only when a single read brought more than the buffer holds
    memcpy(sh->ahead + sh->ahead_len, buf, n);
    sh->ahead_len += n;
}

// --- UTF-8 in the line ----------------------------------------------------------------------------
// The line holds UTF-8 (a Wi-Fi network named "Mjölnir", say). A character's bytes arrive one by
// one; a byte that can't continue or start a character is dropped, as are control bytes.

// How many bytes a character with this lead byte has: 1 for ASCII, 2 to 4, 0 if it can't lead.
static unsigned utf8_len(unsigned char c) {
    if (c < 0x80)
        return 1;
    if (c >= 0xc2 && c <= 0xdf)
        return 2;
    if (c >= 0xe0 && c <= 0xef)
        return 3;
    if (c >= 0xf0 && c <= 0xf4)
        return 4;
    return 0;                           // a continuation byte, or never in UTF-8
}

// Where the line's last character starts; how many bytes it should have goes to *want.
static size_t last_char(const shell_t *sh, unsigned *want) {
    size_t i = sh->line_len;
    while (i > 0 && ((unsigned char)sh->line[i - 1] & 0xc0) == 0x80 && sh->line_len - i < 3)
        i--;
    if (i == 0) {                       // only continuation bytes: can't happen, but be safe
        *want = 0;
        return 0;
    }
    i--;
    *want = utf8_len((unsigned char)sh->line[i]);
    return i;
}

// Is the line's last character still missing bytes?
static bool last_char_partial(const shell_t *sh) {
    if (!sh->line_len)
        return false;
    unsigned want;
    size_t start = last_char(sh, &want);
    return want > sh->line_len - start;
}

// Take a byte of a printable character into the line, echoing it unless secret.
static void line_add(shell_t *sh, unsigned char c) {
    unsigned need = utf8_len(c);
    if ((c & 0xc0) == 0x80) {           // continuation: only of a character under way
        if (!last_char_partial(sh))
            return;
    } else if (need == 0) {             // 0xc0, 0xc1, 0xf5-0xff: never in UTF-8
        return;
    } else {
        if (last_char_partial(sh)) {    // a new character before the last one ended: drop that
            unsigned want;
            sh->line_len = last_char(sh, &want);
        }
        if (sh->line_len + need > SHELL_LINE_MAX)   // the whole character must fit
            return;
    }
    history_edited(sh);
    sh->line[sh->line_len++] = (char)c;
    if (!sh->secret)
        write_out(sh, (const char *)&c, 1);
}

// Backspace: remove the line's last character, and as many columns of it on screen.
static void line_backspace(shell_t *sh) {
    if (!sh->line_len)
        return;
    history_edited(sh);
    unsigned want;
    size_t start = last_char(sh, &want);
    size_t len = sh->line_len - start;
    const unsigned char *p = (const unsigned char *)sh->line + start;
    int width = 1;
    if (want >= 2 && len < want)        // cut short: the terminal showed nothing yet
        width = 0;
    else if (want >= 2) {
        uint32_t cp = p[0] & (0x7f >> want);
        for (size_t i = 1; i < len; i++)
            cp = cp << 6 | (p[i] & 0x3f);
        width = charwidth(cp);
    }
    sh->line_len = start;
    if (!sh->secret)
        for (int i = 0; i < width; i++)
            shell_printf(sh, "\b \b");
}

// --- Tab completion ------------------------------------------------------------------------------
// Tab completes the word at the end of the line: a command, a word that may follow it, or a
// file name on the SD card (complete.h). One match: the rest of it, then a space, or '/' for a
// directory. Several: as far as they agree; when they agree no further, the terminal's bell,
// and a second Tab lists them.

#define TAB_LIST_COLUMNS 80     // the terminal's width isn't known: list for the narrowest

// Columns the `len` bytes at `text` take on the terminal (valid UTF-8: complete.h).
static size_t text_columns(const char *text, size_t len) {
    size_t cols = 0;
    const unsigned char *p = (const unsigned char *)text, *end = p + len;
    while (p < end) {
        unsigned n = utf8_len(*p);
        if (n <= 1) {
            cols++;
            p++;
            continue;
        }
        if (p + n > end)                // cut short: the terminal shows nothing of it yet
            break;
        uint32_t cp = p[0] & (0x7f >> n);
        for (unsigned i = 1; i < n; i++)
            cp = cp << 6 | (p[i] & 0x3f);
        cols += (size_t)charwidth(cp);
        p += n;
    }
    return cols;
}

static char ascii_lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + 'a' - 'A') : c;
}

typedef struct {
    char first[SHELL_LINE_MAX + 1];     // the first match: a copy (a file name's buffer is reused)
    bool have;
    char first_end;             // what follows it: ' ', '/' or nothing ('\0')
    size_t common;              // bytes all matches have in common
    size_t widest;              // columns of the widest, with its '/'
    bool notes;                 // some match has a note: list one a line, the note beside it
} tab_matches_t;

static void tab_collect(const char *word, char end, const char *note, void *arg) {
    tab_matches_t *m = arg;
    if (note && *note)
        m->notes = true;
    size_t n = strlen(word);
    if (!m->have) {
        memcpy(m->first, word, n + 1);  // fits: complete.h offers no longer word
        m->have = true;
        m->first_end = end;
        m->common = n;
    } else {
        size_t i = 0;               // file names match in any ASCII case (complete.h)
        while (i < m->common && ascii_lower(word[i]) == ascii_lower(m->first[i]))
            i++;
        m->common = i;
    }
    size_t cols = text_columns(word, n) + (end == '/');
    if (cols > m->widest)
        m->widest = cols;
}

typedef struct {
    shell_t *sh;
    size_t width, per_row, in_row, last;   // `last`: columns of the match printed before
} tab_list_t;

// A note from outside (a key comment, an SSID), as termtext shows outside text: nothing in it
// reaches the terminal as a control or an invisible character. Line breaks and tabs become
// spaces, so it stays on its line.
static void tab_note(shell_t *sh, const char *note) {
    termtext_t t = TERMTEXT_INIT;
    char out[32 + 3];
    size_t len = strlen(note);
    for (size_t i = 0; i < len; i += 32) {
        size_t n = len - i < 32 ? len - i : 32;
        size_t got = termtext_filter(&t, (const uint8_t *)note + i, n, out);
        for (size_t j = 0; j < got; j++)
            if (out[j] == '\n' || out[j] == '\t')
                out[j] = ' ';
        write_out(sh, out, got);
    }
    write_out(sh, out, termtext_flush(&t, out));
}

// One match of the list, in columns `width` wide; no spaces at the end of a row. With notes
// (per_row 1), the note after the column.
static void tab_print(const char *word, char end, const char *note, void *arg) {
    tab_list_t *l = arg;
    if (l->in_row == l->per_row) {
        write_out(l->sh, "\n", 1);
        l->in_row = 0;
    } else if (l->in_row) {
        shell_printf(l->sh, "%*s", (int)(l->width - l->last), "");
    }
    l->in_row++;
    write_out(l->sh, word, strlen(word));
    if (end == '/')
        write_out(l->sh, "/", 1);
    l->last = text_columns(word, strlen(word)) + (end == '/');
    if (note && *note) {
        shell_printf(l->sh, "%*s", (int)(l->width - l->last), "");
        tab_note(l->sh, note);
    }
}

// The matches for what's typed: the answer to a question that has a completer (never a secret
// one), or a command line. False if Tab has nothing to complete from here.
static bool tab_source(shell_t *sh, size_t *start, size_t *n, complete_fn fn, void *arg,
                       bool *partial) {
    if (sh->answer) {
        if (!sh->ask_complete || sh->secret)
            return false;
        *n = complete_answer(sh, sh->ask_complete, sh->line, sh->line_len, sh->ask_words,
                             sh->ask_chars, start, fn, arg, partial);
        return true;
    }
    const shell_cmd_t *const *tables = shell_command_tables(sh);
    if (!tables)
        return false;
    complete_src_t src = {tables, sh->io->admin, shell_list_dir, sh};
    *n = complete_words(&src, sh->line, sh->line_len, start, fn, arg, partial);
    return true;
}

static void line_tab(shell_t *sh, bool again) {
    size_t start, n;
    bool partial;
    tab_matches_t m = {0};
    if (!tab_source(sh, &start, &n, tab_collect, &m, &partial))
        return;
    size_t typed = sh->line_len - start;
    if (n == 0) {
        write_out(sh, "\a", 1);
        return;
    }
    // Two names can share the first bytes of different characters: stop before those.
    while (m.common > typed && ((unsigned char)m.first[m.common] & 0xc0) == 0x80)
        m.common--;
    // A directory cut short may hold more matches, which needn't share what these do (dev-1:
    // "f29" in 300 files read 256 of them and would have become "f29.txt", with f290.txt to
    // f299.txt unread): then don't add to the line at all, only list.
    bool whole = n == 1 && !partial;
    if ((m.common > typed && !partial) || whole) {
        bool end = whole && m.first_end;            // a single match ends with ' ' or '/'
        size_t add = m.common - typed + end;
        if (sh->line_len + add > SHELL_LINE_MAX) {
            write_out(sh, "\a", 1);
            return;
        }
        history_edited(sh);
        // A file name typed in another case: write it again in the name's own. The bytes and
        // the columns are the same (only ASCII letters differ), so it covers what was there.
        if (memcmp(sh->line + start, m.first, typed)) {
            size_t cols = text_columns(sh->line + start, typed);
            for (size_t i = 0; i < cols; i++)
                write_out(sh, "\b", 1);
            memcpy(sh->line + start, m.first, typed);
            write_out(sh, sh->line + start, typed);
        }
        memcpy(sh->line + sh->line_len, m.first + typed, m.common - typed);
        if (end)
            sh->line[sh->line_len + add - 1] = m.first_end;
        write_out(sh, sh->line + sh->line_len, add);
        sh->line_len += add;
        return;
    }
    if (!again) {
        sh->tab_pressed = true;
        write_out(sh, "\a", 1);
        return;
    }
    tab_list_t l = {.sh = sh, .width = m.widest + 2};
    l.per_row = m.notes ? 1 : TAB_LIST_COLUMNS / l.width;
    if (!l.per_row)
        l.per_row = 1;
    write_out(sh, "\n", 1);
    tab_source(sh, &start, &n, tab_print, &l, &partial);
    if (partial)
        shell_printf(sh, sh->answer ? "\n(more may come: Tab again in a moment)"
                                    : "\n(more: the directory is too large to list here)");
    write_out(sh, "\n", 1);
    prompt(sh);
    write_out(sh, sh->line, sh->line_len);
}

// Line editing for `n` input bytes. Stops early (stashing the rest) if a command starts a job.
static void process_input(shell_t *sh, const char *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n' && sh->last_was_cr) {      // swallow LF of a CRLF pair
            sh->last_was_cr = false;
            continue;
        }
        sh->last_was_cr = (c == '\r');

        int k = keys_feed(&sh->keys, (unsigned char)c);
        bool tab_again = sh->tab_pressed;       // any other key in between starts over
        sh->tab_pressed = false;
        if (k == '\t') {
            if (!sh->answer || (sh->ask_complete && !sh->secret))    // never at a password
                line_tab(sh, tab_again);
            continue;
        }
        if (k == KEY_UP || k == KEY_DOWN) {
            history_key(sh, k);
            continue;
        }
        if (k == KEYS_NONE || k > 0xff)         // other special keys: no editing with them (yet)
            continue;
        if (c == '\r' || c == '\n') {
            history_edited(sh);
            shell_printf(sh, "\n");
            if (last_char_partial(sh)) {        // a character cut short: not part of the line
                unsigned want;
                sh->line_len = last_char(sh, &want);
            }
            sh->line[sh->line_len] = '\0';
            sh->line_len = 0;
            if (!answer_question(sh, sh->line))
                run_line(sh, sh->line);
            if (sh->job) {  // the command started a job: keep the rest for afterwards, unless
                if (!sh->pipe.paging)       // the pager has the screen (not keys for it)
                    stash(sh, buf + i + 1, n - (i + 1));
                return;
            }
            prompt(sh);
        } else if (c == 0x7f || c == '\b') {
            line_backspace(sh);
        } else if (c == 0x03) {                  // Ctrl-C: discard line, cancel a question
            history_edited(sh);
            shell_printf(sh, "^C\n");
            sh->line_len = 0;
            answer_question(sh, NULL);
            prompt(sh);
        } else if (c == 0x04) {                  // Ctrl-D on an empty line: logout
            if (sh->line_len == 0) {
                shell_printf(sh, "\n");
                if (sh->answer && sh->ask_eof) {  // ...or end of input for the question
                    answer_question(sh, SHELL_EOF);
                    if (sh->job) {
                        stash(sh, buf + i + 1, n - (i + 1));
                        return;
                    }
                } else {
                    shell_logout(sh);
                }
                prompt(sh);
            }
        } else if ((unsigned char)c >= 0x20 && c != 0x7f) {
            line_add(sh, (unsigned char)c);
        }
    }
}

// Raw input mode: decode the bytes into keys for the job. Enter is '\r' whichever of CR, LF or
// CRLF the terminal sends.
static void raw_keys(shell_t *sh, const char *buf, size_t n) {
    for (size_t i = 0; i < n && sh->key; i++) {
        char c = buf[i];
        if (c == '\n' && sh->last_was_cr) {
            sh->last_was_cr = false;
            continue;
        }
        sh->last_was_cr = (c == '\r');
        int k = keys_feed(&sh->keys, (unsigned char)c);
        if (k == KEYS_NONE)
            continue;
        sh->key(sh, k == '\n' ? '\r' : k, sh->job_arg);
    }
}

// While a raw-mode job runs: up to RAW_PASS_MAX input bytes go to the job as keys, then one step.
#define RAW_PASS_MAX 256

static void poll_raw(shell_t *sh) {
    if (sh->ahead_len) {        // typed after the command that started the job, in the same read
        size_t len = sh->ahead_len;
        sh->ahead_len = 0;
        raw_keys(sh, sh->ahead, len);
    }
    char buf[32];
    int n;
    size_t total = 0;
    while (total < RAW_PASS_MAX && (n = sh->io->read(sh->ctx, buf, sizeof(buf))) > 0) {
        raw_keys(sh, buf, (size_t)n);
        total += (size_t)n;
    }
    if (!sh->job)               // key() must not end the job, but don't call NULL if it did
        return;
    if (!sh->job(sh, sh->job_arg, false)) {
        sh->job = NULL;
        sh->job_arg = NULL;
        sh->key = NULL;
        if (sh->pipe.released) {            // the pager left: its producer asked a question
            pipe_resume(sh);
            if (sh->job)
                return;
        }
        if (!sh->at_line_start)
            write_out(sh, "\n", 1);
        prompt(sh);
    }
}

// While a job runs: Ctrl-C cancels it (and drops input typed meanwhile); other input is kept.
static void poll_job(shell_t *sh) {
    char buf[32];
    int n;
    bool cancel = false;
    while (!cancel && sh->ahead_len < sizeof(sh->ahead)) {
        size_t room = sizeof(sh->ahead) - sh->ahead_len;
        if ((n = sh->io->read(sh->ctx, buf, room < sizeof(buf) ? (int)room : (int)sizeof(buf))) <= 0)
            break;
        for (int i = 0; i < n && !cancel; i++) {
            if (buf[i] == 0x03)
                cancel = true;
            else
                stash(sh, &buf[i], 1);
        }
    }
    if (cancel) {
        sh->ahead_len = 0;
        if (sh->pipe.pending)               // a pipeline: held and captured output goes, ^C
            pipe_drop(sh);                  // shows
        shell_printf(sh, "%s^C\n", sh->at_line_start ? "" : "\n");
    }
    bool more = sh->job(sh, sh->job_arg, cancel);
    if (more && !cancel && sh->pipe.pending && sh->pipe.done) {
        end_job_quietly(sh);                // head has its lines, or the capture is full
        more = false;
    }
    if (more && !cancel && page_now(sh)) {  // `| more` has its first output: page it now
        page_early(sh);
        return;
    }
    if (!more || cancel) {
        sh->job = NULL;
        sh->job_arg = NULL;
        if (sh->pipe.pending) {             // the line ends: with `more` the pager takes over
            if (sh->answer) {               // unless the job left a question: not piped
                pipe_release(sh);
            } else {
                if (sh->pipe.sink == PIPE_SINK_PAGER)
                    sh->ahead_len = 0;      // typed meanwhile: not keys for the pager
                pipe_finish(sh);
                if (sh->job)
                    return;
            }
        }
        if (!sh->at_line_start)
            write_out(sh, "\n", 1);
        prompt(sh);
    }
}

void shell_poll(shell_t *sh) {
    char buf[32];
    int n;
    if (sh->job) {
        if (sh->key)
            poll_raw(sh);
        else
            poll_job(sh);
        return;
    }
    if (sh->ahead_len) {        // input typed during the last job
        char ahead[sizeof(sh->ahead)];
        size_t len = sh->ahead_len;
        memcpy(ahead, sh->ahead, len);
        sh->ahead_len = 0;
        // Typed-ahead input can be a password (typed while a check was running): don't leave
        // copies of it behind.
        memset(sh->ahead, 0, len);
        process_input(sh, ahead, len);
        volatile char *v = ahead;
        for (size_t i = 0; i < len; i++)
            v[i] = 0;
        if (sh->job)
            return;
    }
    while (!sh->job && (n = sh->io->read(sh->ctx, buf, sizeof(buf))) > 0)
        process_input(sh, buf, (size_t)n);
}
