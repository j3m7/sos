// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The console's lock (see login.h).

#include "shell/login.h"

#include <string.h>

static const login_hooks_t *hooks;
static bool locked = true;              // locked until login_start() decides otherwise
static unsigned failures;               // wrong answers since boot
static uint64_t not_before;             // no check starts before this time (ms)
static char pending[LOGIN_PASSWORD_MAX + 1];    // the answer, from typed until its check starts
static bool have_pending, checking;

static void wipe(void *p, size_t len) {
    volatile char *v = p;
    while (len--)
        *v++ = 0;
}

// After the nth wrong answer in a row since boot: 1 s, 2 s, 4 s ... up to 30 s. It can't be
// kept across a reboot (a counter in flash could be rewritten), and doesn't need to be: a
// reboot takes longer than the first delays, and every check costs its slow derivation anyway.
static uint32_t delay_ms(unsigned n) {
    if (n == 0)
        return 0;
    return n > 5 ? 30000 : 1000u << (n - 1);
}

static void ask(shell_t *sh);

static void wrong(shell_t *sh) {
    failures++;
    not_before = hooks->now_ms() + delay_ms(failures);
    if (hooks->failed)
        hooks->failed(failures);
    shell_printf(sh, "wrong password\n");
    ask(sh);
}

// The job that checks an answer: wait out the delay, then the check in slices.
static bool check_job(shell_t *sh, void *arg, bool cancel) {
    (void)arg;
    if (cancel) {
        if (checking)
            hooks->check_abort();
        checking = false;
        have_pending = false;
        wipe(pending, sizeof(pending));
        ask(sh);
        return false;
    }
    if (!checking) {
        if (hooks->now_ms() < not_before)
            return true;
        bool started = have_pending && hooks->check_begin(pending);
        have_pending = false;
        wipe(pending, sizeof(pending));
        if (!started) {
            wrong(sh);
            return false;
        }
        checking = true;
        return true;
    }
    int r = hooks->check_step();
    if (r == LOGIN_MORE)
        return true;
    checking = false;
    if (r != LOGIN_RIGHT) {
        wrong(sh);
        return false;
    }
    locked = false;
    failures = 0;
    not_before = 0;
    hooks->opened(sh);
    return false;
}

static void on_password(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (!locked)
        return;
    if (!answer || !*answer) {          // Ctrl-C or an empty line: just ask again
        ask(sh);
        return;
    }
    size_t n = strlen(answer);
    have_pending = n <= LOGIN_PASSWORD_MAX;     // too long to be a password: wrong, unchecked
    if (have_pending)
        memcpy(pending, answer, n + 1);
    wipe(answer, n);
    shell_run_job(sh, check_job, NULL);
}

static void ask(shell_t *sh) {
    shell_ask(sh, "Password: ", true, on_password, NULL);
}

void login_start(shell_t *console, const login_hooks_t *h) {
    hooks = h;
    failures = 0;
    not_before = 0;
    have_pending = checking = false;
    const char *why = hooks->no_keys();
    if (why) {
        locked = true;
        shell_printf(console, "\nThe console is locked: %s\n", why);
        shell_show_input(console);
        return;
    }
    if (hooks->has_password()) {
        locked = true;
        shell_printf(console, "\n");
        ask(console);
        shell_show_input(console);
        return;
    }
    locked = false;
    hooks->first_use(console);
    shell_show_input(console);
}

bool login_locked(void) {
    return locked;
}

unsigned login_failures(void) {
    return failures;
}

void login_lock(shell_t *console) {
    if (hooks->no_keys()) {
        locked = true;
        return;
    }
    if (!hooks->has_password()) {
        shell_printf(console, "no console password is set, so the console can't be locked "
                              "(`passwd` sets one)\n");
        return;
    }
    locked = true;
    shell_printf(console, "console locked\n");
    ask(console);
}

bool login_gate(shell_t *console, const char *cmd) {
    if (!locked)
        return false;
    const char *why = hooks->no_keys();
    if (why) {
        if (hooks->rescue_allowed(cmd))
            return false;
        shell_printf(console, "The console is locked: %s\n", why);
        return true;
    }
    // A command line reached the shell while locked (the question was dropped somehow): it
    // doesn't run, and the question is put back.
    ask(console);
    return true;
}
