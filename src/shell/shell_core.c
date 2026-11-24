// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Shell line editing and output. Depends only on shell.h and the C library, so it builds on the
// host for unit tests; the commands live in shell.c (shell_execute).

#include "shell/shell.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// --- output --------------------------------------------------------------------

static void write_out(shell_t *sh, const char *buf, size_t len) {
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
    shell_printf(sh, "%s", sh->answer ? sh->ask_prompt : "sos> ");
}

void shell_ask(shell_t *sh, const char *ask_prompt, bool secret, shell_answer_fn answer, void *arg) {
    sh->ask_prompt = ask_prompt;
    sh->secret = secret;
    sh->answer = answer;
    sh->answer_arg = arg;
}

// Take the pending question, if any, and hand it the answer (NULL = cancelled).
static bool answer_question(shell_t *sh, char *answer) {
    shell_answer_fn fn = sh->answer;
    if (!fn)
        return false;
    void *arg = sh->answer_arg;
    sh->answer = NULL;                  // cleared first: the callback may ask the next question
    sh->secret = false;
    fn(sh, answer, arg);
    return true;
}

void shell_hide_input(shell_t *sh) {
    if (!sh->io)
        return;
    if (sh->job) {                              // job output on screen: don't erase it
        if (!sh->at_line_start)
            write_out(sh, "\n", 1);
        return;
    }
    sh->io->write(sh->ctx, "\r\x1b[K", 4);    // to column 0, erase the line
}

void shell_show_input(shell_t *sh) {
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

// Trim spaces and hand non-empty commands to the command module.
static void run_line(shell_t *sh, char *cmd) {
    while (*cmd == ' ')
        cmd++;
    char *end = cmd + strlen(cmd);
    while (end > cmd && end[-1] == ' ')
        *--end = '\0';
    if (*cmd)
        shell_execute(sh, cmd);
}

// --- input ---------------------------------------------------------------------

// Terminals send special keys as escape sequences, which must not end up in the line:
// CSI "ESC [ params final" (arrows, Delete, Ctrl+arrow, bracketed paste), SS3 "ESC O x"
// (application-mode arrows, F1-F4) and "ESC x" (Alt+key). They may be split across reads,
// so the state lives in shell_t. A control character cancels a sequence and is handled
// normally, so a stray ESC can't swallow Enter or Ctrl-C.
enum { ESC_NONE, ESC_START, ESC_CSI, ESC_SS3 };

// Returns true if c was consumed as part of an escape sequence.
static bool escape_sequence(shell_t *sh, unsigned char c) {
    if (c == 0x1b) {
        sh->esc = ESC_START;
        return true;
    }
    if (sh->esc == ESC_NONE)
        return false;
    if (c < 0x20) {                         // control character: abandon the sequence
        sh->esc = ESC_NONE;
        return false;
    }
    switch (sh->esc) {
    case ESC_START:
        sh->esc = c == '[' ? ESC_CSI : c == 'O' ? ESC_SS3 : ESC_NONE;
        break;
    case ESC_CSI:                           // parameter/intermediate bytes until a final byte
        if (c >= 0x40 && c <= 0x7e)
            sh->esc = ESC_NONE;
        break;
    default:                                // SS3: one byte
        sh->esc = ESC_NONE;
        break;
    }
    return true;
}

void shell_init(shell_t *sh, const shell_io_t *io, void *ctx) {
    sh->io = io;
    sh->ctx = ctx;
    sh->line_len = 0;
    sh->last_was_cr = false;
    sh->esc = ESC_NONE;
    sh->answer = NULL;
    sh->secret = false;
    sh->job = NULL;             // a job of an earlier session was cancelled by shell_end()
    sh->job_arg = NULL;
    sh->ahead_len = 0;
    strcpy(sh->cwd, "/");
    shell_printf(sh, "\nType 'help' for commands.\n");
    prompt(sh);
}

void shell_run_job(shell_t *sh, shell_job_fn step, void *arg) {
    sh->job = step;
    sh->job_arg = arg;
}

bool shell_job_running(const shell_t *sh) {
    return sh->job != NULL;
}

bool shell_busy(const shell_t *sh) {
    return sh->job != NULL || sh->ahead_len > 0;
}

int shell_write_space(shell_t *sh) {
    if (!sh->io->write_space)
        return 1024;
    int n = sh->io->write_space(sh->ctx);
    return n > 0 ? n : 0;
}

void shell_end(shell_t *sh) {
    if (sh->job) {
        shell_job_fn step = sh->job;
        sh->job = NULL;
        step(sh, sh->job_arg, true);
    }
    sh->answer = NULL;
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

// Line editing for `n` input bytes. Stops early (stashing the rest) if a command starts a job.
static void process_input(shell_t *sh, const char *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n' && sh->last_was_cr) {      // swallow LF of a CRLF pair
            sh->last_was_cr = false;
            continue;
        }
        sh->last_was_cr = (c == '\r');

        if (escape_sequence(sh, (unsigned char)c))
            continue;
        if (c == '\r' || c == '\n') {
            shell_printf(sh, "\n");
            sh->line[sh->line_len] = '\0';
            sh->line_len = 0;
            if (!answer_question(sh, sh->line))
                run_line(sh, sh->line);
            if (sh->job) {  // the command started a job: keep the rest for afterwards
                stash(sh, buf + i + 1, n - (i + 1));
                return;
            }
            prompt(sh);
        } else if (c == 0x7f || c == '\b') {
            if (sh->line_len > 0) {
                sh->line_len--;
                if (!sh->secret)
                    shell_printf(sh, "\b \b");
            }
        } else if (c == 0x03) {                  // Ctrl-C: discard line, cancel a question
            shell_printf(sh, "^C\n");
            sh->line_len = 0;
            answer_question(sh, NULL);
            prompt(sh);
        } else if (c == 0x04) {                  // Ctrl-D on an empty line: logout
            if (sh->line_len == 0) {
                shell_printf(sh, "\n");
                shell_logout(sh);
                prompt(sh);
            }
        } else if (c >= 0x20 && c < 0x7f && sh->line_len < SHELL_LINE_MAX) {
            sh->line[sh->line_len++] = c;
            if (!sh->secret)
                write_out(sh, &c, 1);
        }
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
        shell_printf(sh, "%s^C\n", sh->at_line_start ? "" : "\n");
    }
    if (!sh->job(sh, sh->job_arg, cancel) || cancel) {
        sh->job = NULL;
        sh->job_arg = NULL;
        if (!sh->at_line_start)
            write_out(sh, "\n", 1);
        prompt(sh);
    }
}

void shell_poll(shell_t *sh) {
    char buf[32];
    int n;
    if (sh->job) {
        poll_job(sh);
        return;
    }
    if (sh->ahead_len) {        // input typed during the last job
        char ahead[sizeof(sh->ahead)];
        size_t len = sh->ahead_len;
        memcpy(ahead, sh->ahead, len);
        sh->ahead_len = 0;
        process_input(sh, ahead, len);
        if (sh->job)
            return;
    }
    while (!sh->job && (n = sh->io->read(sh->ctx, buf, sizeof(buf))) > 0)
        process_input(sh, buf, (size_t)n);
}
