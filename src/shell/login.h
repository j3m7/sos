// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The console's lock (issue #64): the serial console asks for the console password before it
// takes any command, like a login. Until then nothing is shown: no greeting with the firmware
// version, no log lines, no messages from other sessions.
//
// - Locked: the shell has a pending question, "Password: ", not echoed. A right answer opens
//   the console; a wrong one asks again, after a delay that grows with each wrong answer since
//   boot (the slow password check is the real limit; the delay only adds to it). Checking runs
//   as a shell job in slices, so the main loop keeps turning.
// - The gate: login_gate() is called for every command line of the console before it runs, and
//   refuses while locked, whatever state the shell's question is in. The lock doesn't depend on
//   the question staying pending.
// - No password set (a new board, or a configuration from before console passwords): the
//   console is open, and login_start() hands over to `first_use`, which starts setup with the
//   password as its first step.
// - No usable root secret: no password can be checked, and the console must not be open for
//   that reason. It stays locked and takes only the commands that deal with it
//   (login_hooks_t.rescue_allowed).
// - `logout` on the console locks it again.
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
} login_hooks_t;

// At boot, on the console's shell, bound but with nothing printed yet (shell_bind): lock it
// and ask for the password, or open it if none is set.
void login_start(shell_t *console, const login_hooks_t *hooks);

// Is the console locked? (While it is: no log lines, no messages from other sessions.)
bool login_locked(void);

// Lock the console again and ask for the password (`logout` on the console). Says so instead
// if no password is set.
void login_lock(shell_t *console);

// For every command line of the console, before it runs: true if it must not run (locked).
// Not for other shells.
bool login_gate(shell_t *console, const char *cmd);

// Wrong answers since boot (for `who`-style reporting and tests).
unsigned login_failures(void);

#endif
