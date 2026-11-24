// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_SHELL_H
#define SOS_SHELL_H

#include <stdbool.h>
#include <stddef.h>

// Minimal line-oriented shell. Each instance has its own line buffer and talks to one channel
// (the USB/UART console, an SSH session, ...) through a shell_io_t, so sessions don't share
// input or output.

#define SHELL_LINE_MAX 256     // fits a pasted authorized_keys line (ECDSA P-256 is ~200)
#define SHELL_PATH_MAX 128     // working directory of the file commands

typedef struct {
    const char *name;                                   // channel name, e.g. "console", "ssh"
    int (*read)(void *ctx, char *buf, int len);         // non-blocking; bytes read, 0 if none
    void (*write)(void *ctx, const char *buf, int len);
    void (*logout)(void *ctx);                          // end the session; NULL if not possible
    bool crlf;                                          // translate "\n" to "\r\n" on output
    bool admin;                                         // may change the configuration
    // Bytes write() can take now without waiting; NULL if it never waits for long.
    int (*write_space)(void *ctx);
} shell_io_t;

typedef struct shell shell_t;

// Receives the next line typed after shell_ask(): as typed (not trimmed), or NULL if the
// question was cancelled with Ctrl-C.
typedef void (*shell_answer_fn)(shell_t *sh, char *answer, void *arg);

// One step of a long-running command (shell_run_job). Do a bounded amount of work and return
// true to be called again, or false when finished. With `cancel` (Ctrl-C, or the session ended),
// clean up, free `arg` if needed and return false.
typedef bool (*shell_job_fn)(shell_t *sh, void *arg, bool cancel);

struct shell {
    const shell_io_t *io;
    void *ctx;
    char line[SHELL_LINE_MAX + 1];
    size_t line_len;
    bool last_was_cr;
    unsigned char esc;          // escape sequence state (arrow keys etc.), see shell_core.c
    // Pending question (shell_ask): the next line goes to `answer` instead of a command.
    shell_answer_fn answer;
    void *answer_arg;
    const char *ask_prompt;
    bool secret;                // don't echo the answer (passwords)
    bool ask_eof;               // Ctrl-D on an empty line answers SHELL_EOF (see shell_ask_eof)
    // Running job (shell_run_job): stepped by shell_poll; no prompt or command input meanwhile.
    shell_job_fn job;
    void *job_arg;
    bool at_line_start;         // last output ended with a newline
    char ahead[SHELL_LINE_MAX]; // input typed while a job ran, processed when it ends
    size_t ahead_len;
    char cwd[SHELL_PATH_MAX];   // working directory for file commands ("/" after init)
};

// Bind a shell to a channel and print the greeting and first prompt.
void shell_init(shell_t *sh, const shell_io_t *io, void *ctx);

// Process any input waiting on the channel, or run the next step of a job. Never blocks; call
// regularly from the main loop.
void shell_poll(shell_t *sh);

// printf-style output to the shell's channel.
void shell_printf(shell_t *sh, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
// Raw output (any bytes, any length), with the channel's newline translation.
void shell_write(shell_t *sh, const char *buf, size_t len);

// End the session through io->logout, or say that this channel can't be ended.
void shell_logout(shell_t *sh);

// Run one command line (spaces trimmed, never empty). Implemented by the command module
// (shell.c); shell_core.c (line editing) calls it.
void shell_execute(shell_t *sh, char *cmd);

// Ask a question: `prompt` replaces "sos> " and the next line typed is passed to `answer`
// instead of being run as a command. With `secret`, the answer isn't echoed. Never blocks: call
// it from a command (or from an answer callback, to chain questions) and return. `prompt` is
// kept, not copied (it's redrawn around log lines), so it must stay valid until answered: use a
// string literal or a static buffer.
void shell_ask(shell_t *sh, const char *prompt, bool secret, shell_answer_fn answer, void *arg);

// Like shell_ask (echoed), for questions that take a stream of lines, such as an interactive
// prompt: Ctrl-D on an empty line answers SHELL_EOF ("end of input") instead of logging out.
// Compare the answer's pointer with SHELL_EOF; it reads as "".
void shell_ask_eof(shell_t *sh, const char *prompt, shell_answer_fn answer, void *arg);
extern char shell_eof[];
#define SHELL_EOF shell_eof

// For output that isn't the shell's own (log messages): clear the line being typed, and
// afterwards redraw the prompt and the typed text (except a secret answer).
void shell_hide_input(shell_t *sh);
void shell_show_input(shell_t *sh);

// Run a long command in steps instead of blocking the main loop: `step` is called from
// shell_poll until it returns false; then the prompt is shown. Call it from a command. Input
// typed meanwhile is kept and processed afterwards (like a terminal's type-ahead); Ctrl-C
// cancels the job and drops it.
void shell_run_job(shell_t *sh, shell_job_fn step, void *arg);
bool shell_job_running(const shell_t *sh);
// A job is running or typed-ahead input is waiting to run.
bool shell_busy(const shell_t *sh);

// Bytes of output the channel takes now without waiting (jobs size their output by it).
int shell_write_space(shell_t *sh);

// The channel is gone (session ended): cancel a running job so it frees its resources.
void shell_end(shell_t *sh);

#endif
