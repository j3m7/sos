// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The console's lock (issue #64): the serial console asks for the console password before it
// takes any command, like a login. Until then nothing is shown: no greeting with the firmware
// version, no log lines, no messages from other sessions.
//
// - Locked: the shell has a pending question, "Password: ", not echoed. A right answer opens
//   the console; a wrong one asks again, after a delay that grows with each wrong answer since
//   the last right one (the slow password check is the real limit; the delay only adds to it).
//   Checking runs as a shell job in slices, so the main loop keeps turning.
// - The count of wrong answers can be kept across reboots (the three hooks at the end of
//   login_hooks_t; core/failcount.h on the board). An answer is counted BEFORE its check starts
//   and the count is cleared after a right one, so a check cut short by a reset, or by Ctrl-C,
//   counts as a wrong answer, and a board that was reset starts with the delay it had earned.
// - The gate: login_gate() is called for every command line of the console before it runs, and
//   refuses while locked, whatever state the shell's question is in. The lock doesn't depend on
//   the question staying pending.
// - No password set (a new board, or a configuration from before console passwords): the
//   console is open, and login_start() hands over to `first_use`, which starts setup with the
//   password as its first step.
// - No usable root secret: no password can be checked, and the console must not be open for
//   that reason. It stays locked and takes only the commands that deal with it
//   (login_hooks_t.rescue_allowed).
// - `logout` on the console locks it again. So does the console itself (login_poll): when the
//   USB connection to the terminal drops, and after a time with nothing typed. Whoever plugs in
//   next, or walks up later, finds a locked console. A command that was still running (a long
//   listing, the editor) is ended, and text typed but not entered is thrown away.
//
// Depends only on shell.h and its hooks, so it's unit tested on the host with the real shell
// line editor. SSH sessions are not involved: they log in with their keys.

#ifndef SOS_LOGIN_H
#define SOS_LOGIN_H

#include <stdbool.h>
#include <stdint.h>

#include "shell/shell.h"

#define LOGIN_PASSWORD_MAX 64           // longer answers are wrong without being checked

enum { LOGIN_MORE, LOGIN_RIGHT, LOGIN_WRONG };

typedef struct {
    uint64_t (*now_ms)(void);
    // Is a console password set?
    bool (*has_password)(void);
    // Start checking a password (it isn't kept: the caller wipes it). False if it can't start.
    bool (*check_begin)(const char *password);
    // A bounded amount of the check: LOGIN_MORE until it's decided.
    int (*check_step)(void);
    // Drop a check that was started (the user pressed Ctrl-C).
    void (*check_abort)(void);
    // NULL when the board has its keys. Otherwise why not, in a sentence: the console then
    // stays locked whatever else is true.
    const char *(*no_keys)(void);
    // With no keys: may this command line run anyway (the ones that put it right)?
    bool (*rescue_allowed)(const char *cmd);
    // The console was just opened with the password: greet.
    void (*opened)(shell_t *sh);
    // The console is open because no password is set: start setting one.
    void (*first_use)(shell_t *sh);
    // A wrong answer was given (`failures` since the last right one, this one included): for
    // the log. May be NULL.
    void (*failed)(unsigned failures);
    // The console locked itself (`why`: a few words): for the log. May be NULL.
    void (*auto_locked)(const char *why);
    // The count kept across reboots. All three may be NULL: the count then starts at zero at
    // every boot.
    // - At boot: answers counted and not followed by a right one.
    unsigned (*kept_failures)(void);
    // - An answer is about to be checked (or was refused unchecked): count it, durably, now.
    void (*count_attempt)(void);
    // - The answer was right: the count is zero again.
    void (*clear_count)(void);
} login_hooks_t;

// At boot, on the console's shell, bound but with nothing printed yet (shell_bind): lock it
// and ask for the password, or open it if none is set.
void login_start(shell_t *console, const login_hooks_t *hooks);

// Is the console locked? (While it is: no log lines, no messages from other sessions.)
bool login_locked(void);

// Lock the console again and ask for the password (`logout` on the console). Says so instead
// if no password is set.
void login_lock(shell_t *console);

// Something was typed on the console (any byte): restarts the idle time.
void login_activity(void);

// From the main loop. `connected`: a terminal is connected (USB: the host has the port open; a
// channel that can't tell says true). Locks an open console that has a password when
// `connected` goes from true to false, or when nothing was typed for `idle_limit_ms` (0: no
// idle lock).
void login_poll(shell_t *console, bool connected, uint32_t idle_limit_ms);

// For every command line of the console, before it runs: true if it must not run (locked).
// Not for other shells.
bool login_gate(shell_t *console, const char *cmd);

// Wrong answers since the last right one, as counted now (for reporting and tests).
unsigned login_failures(void);

#endif
