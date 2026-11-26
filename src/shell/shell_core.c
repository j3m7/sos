// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Shell line editing and output. Depends only on shell.h, sos_version and the C library, so it
// builds on the host for unit tests (which define sos_version); the commands live in shell.c
// (shell_execute).

#include "shell/shell.h"
#include "core/version.h"
#include "shell/charwidth.h"

#include <stdarg.h>
#include <stdint.h>
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
    shell_printf(sh, "%s", sh->answer ? sh->ask_prompt : SHELL_PROMPT);
}

void shell_ask(shell_t *sh, const char *ask_prompt, bool secret, shell_answer_fn answer, void *arg) {
    sh->ask_prompt = ask_prompt;
    sh->secret = secret;
    sh->ask_eof = false;
    sh->answer = answer;
    sh->answer_arg = arg;
}

char shell_eof[] = "";

void shell_ask_eof(shell_t *sh, const char *ask_prompt, shell_answer_fn answer, void *arg) {
    shell_ask(sh, ask_prompt, false, answer, arg);
    sh->ask_eof = true;
}

void shell_discard_input(shell_t *sh) {
    volatile char *v = sh->line;
    for (size_t i = 0; i < sizeof(sh->line); i++)
        v[i] = 0;
    v = sh->ahead;
    for (size_t i = 0; i < sizeof(sh->ahead); i++)
        v[i] = 0;
    sh->line_len = 0;
    sh->ahead_len = 0;
    sh->last_was_cr = false;
    sh->keys = (keys_t)KEYS_INIT;
}

void shell_cancel_question(shell_t *sh) {
    sh->answer = NULL;
    sh->secret = false;
    sh->ask_eof = false;
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
    fn(sh, answer, arg);
    return true;
}

void shell_hide_input(shell_t *sh) {
    if (!sh->io || shell_raw_mode(sh))          // full-screen program: leave its screen alone
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

void shell_bind(shell_t *sh, const shell_io_t *io, void *ctx) {
    sh->io = io;
    sh->ctx = ctx;
    sh->line_len = 0;
    sh->last_was_cr = false;
    sh->keys = (keys_t)KEYS_INIT;
    sh->answer = NULL;
    sh->secret = false;
    sh->ask_eof = false;
    sh->job = NULL;             // a job of an earlier session was cancelled by shell_end()
    sh->job_arg = NULL;
    sh->key = NULL;
    sh->ahead_len = 0;
    strcpy(sh->cwd, "/");
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
        sh->key = NULL;
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
    sh->line[sh->line_len++] = (char)c;
    if (!sh->secret)
        write_out(sh, (const char *)&c, 1);
}

// Backspace: remove the line's last character, and as many columns of it on screen.
static void line_backspace(shell_t *sh) {
    if (!sh->line_len)
        return;
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
        if (k == KEYS_NONE || k > 0xff)         // special keys: no line editing with them (yet)
            continue;
        if (c == '\r' || c == '\n') {
            shell_printf(sh, "\n");
            if (last_char_partial(sh)) {        // a character cut short: not part of the line
                unsigned want;
                sh->line_len = last_char(sh, &want);
            }
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
            line_backspace(sh);
        } else if (c == 0x03) {                  // Ctrl-C: discard line, cancel a question
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
