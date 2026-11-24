// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Shell line editing and output. Depends only on shell.h and the C library, so it builds on the
// host for unit tests; the commands live in shell.c (shell_execute).

#include "shell.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// --- output --------------------------------------------------------------------

static void write_out(shell_t *sh, const char *buf, size_t len) {
    if (!sh->io->crlf) {
        sh->io->write(sh->ctx, buf, (int)len);
        return;
    }
    // Translate "\n" to "\r\n", writing the runs in between in one go.
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            if (i > start)
                sh->io->write(sh->ctx, buf + start, (int)(i - start));
            sh->io->write(sh->ctx, "\r\n", 2);
            start = i + 1;
        }
    }
    if (len > start)
        sh->io->write(sh->ctx, buf + start, (int)(len - start));
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
    if (sh->io)
        sh->io->write(sh->ctx, "\r\x1b[K", 4);    // to column 0, erase the line
}

void shell_show_input(shell_t *sh) {
    if (!sh->io)
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
    shell_printf(sh, "\nType 'help' for commands.\n");
    prompt(sh);
}

void shell_poll(shell_t *sh) {
    char buf[32];
    int n;
    while ((n = sh->io->read(sh->ctx, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
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
}
