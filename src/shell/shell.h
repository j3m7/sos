// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_SHELL_H
#define SOS_SHELL_H

#include <stdbool.h>
#include <stddef.h>

#include "core/log.h"
#include "shell/keys.h"

// Minimal line-oriented shell. Each instance has its own line buffer and talks to one channel
// (the USB/UART console, an SSH session, ...) through a shell_io_t, so sessions don't share
// input or output.

#define SHELL_LINE_MAX 256     // fits a pasted authorized_keys line (ECDSA P-256 is ~200)
#define SHELL_PATH_MAX 128     // working directory of the file commands
#define SHELL_PROMPT "sos> "   // the command prompt (a question replaces it)
#define SHELL_HISTORY_BYTES 1024    // recent commands per session, for Up/Down (in RAM only)

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

// --- commands --------------------------------------------------------------------------------
// The command module keeps its commands in tables: shell_execute() looks a command up by name
// there, and Tab completes the name and the subcommand words from them (shell/complete.h).

#define SHELL_ADMIN  0x01      // offered by Tab only on an admin channel (the command checks too)
#define SHELL_HIDDEN 0x02      // never offered by Tab (an alias, a testing command)
#define SHELL_PATH   0x04      // the words after it are paths on the SD card: Tab completes them
#define SHELL_DIR    0x08      // with SHELL_PATH: paths of directories (Tab offers only those)
#define SHELL_REDIRECT 0x10    // a command: the text after the line's last '>' is a path (echo)
#define SHELL_NOSPACE  0x20    // a word Tab ends with nothing, not a space ("https://")
#define SHELL_VALUES   0x40    // marks a shell_values_t (below); not for other entries

// A word that may follow a command or another word, e.g. "sync" after "time". `words` are the
// words that may follow this one (NULL: none, or free text Tab can't guess). Arrays of them end
// with an entry whose name is NULL.
typedef struct shell_word {
    const char *name;
    const struct shell_word *words;
    unsigned flags;            // SHELL_ADMIN, SHELL_HIDDEN, SHELL_PATH, SHELL_DIR, SHELL_NOSPACE
} shell_word_t;

// Values a word takes that only the running board knows (key numbers, saved networks, hosts).
// `fn` is called with each value and a note to show beside it in Tab's list (NULL for none);
// both are copied before `fn` returns, so they may live in a buffer of the caller's.
typedef void (*shell_value_fn)(const char *value, const char *note, void *arg);
typedef void (*shell_values_fn)(shell_t *sh, shell_value_fn fn, void *arg);

// Point a `words` field at a shell_values_t's `mark` (SHELL_VALUES_OF) to have Tab ask `list`
// for those words. `mark` is an empty word list flagged SHELL_VALUES, so code that only knows
// word lists sees no words.
typedef struct {
    shell_word_t mark;          // {NULL, NULL, SHELL_VALUES}
    shell_values_fn list;
} shell_values_t;

_Static_assert(offsetof(shell_values_t, mark) == 0, "SHELL_VALUES_OF() is cast back to the struct");
#define SHELL_VALUES_INIT(fn) {{NULL, NULL, SHELL_VALUES}, (fn)}
#define SHELL_VALUES_OF(v) (&(v).mark)

// A command: `run` gets the arguments after the name, spaces trimmed; NULL without any. Tables
// end with an entry whose name is NULL.
typedef struct {
    const char *name;
    void (*run)(shell_t *sh, char *args);
    const shell_word_t *words; // words that may follow the name, for Tab
    unsigned flags;            // SHELL_ADMIN, SHELL_HIDDEN, SHELL_PATH, SHELL_DIR, SHELL_REDIRECT
} shell_cmd_t;

// Receives the next line typed after shell_ask(): as typed (not trimmed), or NULL if the
// question was cancelled with Ctrl-C.
typedef void (*shell_answer_fn)(shell_t *sh, char *answer, void *arg);

// One step of a long-running command (shell_run_job). Do a bounded amount of work and return
// true to be called again, or false when finished. With `cancel` (Ctrl-C, or the session ended),
// clean up, free `arg` if needed and return false.
typedef bool (*shell_job_fn)(shell_t *sh, void *arg, bool cancel);

// One key for a job in raw input mode (shell_run_raw): a byte (0x00-0xff, Ctrl-C included) or a
// KEY_* code from keys.h. Enter is always '\r'. For KEY_CURSOR_POS, the row and column are in
// sh->keys.param[0] and [1]. It must not end the job itself (shell_end): let its step return
// false instead.
typedef void (*shell_key_fn)(shell_t *sh, int key, void *arg);

struct shell {
    const shell_io_t *io;
    void *ctx;
    char line[SHELL_LINE_MAX + 1];
    size_t line_len;
    bool last_was_cr;
    keys_t keys;                // escape sequence state (arrow keys etc.), see keys.h
    // Pending question (shell_ask): the next line goes to `answer` instead of a command.
    shell_answer_fn answer;
    void *answer_arg;
    const char *ask_prompt;
    bool secret;                // don't echo the answer (passwords)
    bool ask_eof;               // Ctrl-D on an empty line answers SHELL_EOF (see shell_ask_eof)
    // Running job (shell_run_job): stepped by shell_poll; no prompt or command input meanwhile.
    shell_job_fn job;
    void *job_arg;
    shell_key_fn key;           // raw input mode (shell_run_raw): every key goes to the job
    bool at_line_start;         // last output ended with a newline
    char ahead[SHELL_LINE_MAX]; // input typed while a job ran, processed when it ends
    size_t ahead_len;
    char cwd[SHELL_PATH_MAX];   // working directory for file commands ("/" after init)
    // Command history for Up/Down: the commands run in this session, oldest first, each ending
    // in '\0'. Only commands: never answers to questions (passwords). Wiped when a session
    // starts (shell_bind) or ends (shell_end), when the console locks itself
    // (shell_discard_input) or is logged out of (`logout`), and at login (shell_clear_history
    // from login.c, which also covers lines the gate refused while locked).
    char history[SHELL_HISTORY_BYTES];
    size_t history_len;
    int history_pos;            // entry shown, counted back from the newest (0); -1 = none
    char draft[SHELL_LINE_MAX + 1];     // the line being typed before Up, for Down to give back
    size_t draft_len;
    log_quota_t logger_quota;   // `logger`'s rate limit for this shell
    bool tab_pressed;           // the last key was a Tab that couldn't complete: the next lists
};

// Bind a shell to a channel and print the greeting and first prompt.
void shell_init(shell_t *sh, const shell_io_t *io, void *ctx);

// The two halves, for a channel that must show nothing until someone has logged in (the
// console, shell/login.h): bind without printing anything; and the greeting, which names the
// firmware version. Follow with shell_show_input() for the prompt.
void shell_bind(shell_t *sh, const shell_io_t *io, void *ctx);
void shell_greet(shell_t *sh);

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

// The command tables Tab completes from, NULL-terminated; NULL when this shell takes no command
// now (the console is locked). Implemented by the command module (shell.c).
const shell_cmd_t *const *shell_command_tables(const shell_t *sh);

// Start the console (the admin channel) on a shell that was bound with shell_bind(): ask for
// the console password, or open it if none is set (shell/login.h). Implemented by shell.c.
void shell_console_start(shell_t *console);

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

// Throw away what has been typed: the line being edited, input typed ahead during a job (it
// may be a password) and the command history, all wiped. For when the person at the channel is
// no longer the one who typed it (the console locking itself, shell/login.h).
void shell_discard_input(shell_t *sh);

// Wipe the command history only (from login.c: `logout` on the console, and at login).
void shell_clear_history(shell_t *sh);

// Drop a pending question without answering it (its owner has gone away); the next line is a
// command again. For output around it, use shell_hide_input() / shell_show_input().
void shell_cancel_question(shell_t *sh);

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
// A raw-mode job (shell_run_raw) owns the screen: nothing else should write to this shell.
bool shell_raw_mode(const shell_t *sh);

// Like shell_run_job, in raw input mode, for full-screen programs: every key typed goes to `key`
// as it arrives, decoded (keys.h), instead of being kept for afterwards. Ctrl-C is a key like
// any other and doesn't cancel the job, so the program needs its own way out (its step returns
// false); the session ending still cancels it. `key` is called before `step` in the same pass,
// for about 256 input bytes a pass at most, and may be called before the first step (keys typed
// with the command), so the program's state must be ready when shell_run_raw() returns. Keys that
// arrive in the same pass after the program decided to finish still go to `key`: ignore them.
void shell_run_raw(shell_t *sh, shell_job_fn step, shell_key_fn key, void *arg);
// A job is running or typed-ahead input is waiting to run.
bool shell_busy(const shell_t *sh);

// Bytes of output the channel takes now without waiting (jobs size their output by it).
int shell_write_space(shell_t *sh);

// The channel is gone (session ended): cancel a running job so it frees its resources, and
// wipe the command history.
void shell_end(shell_t *sh);

#endif
