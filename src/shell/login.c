// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The console's lock (see login.h).

#include "shell/login.h"

#include <string.h>

static const login_hooks_t *hooks;
static bool locked = true;              // locked until login_start() decides otherwise
static unsigned failures;               // answers counted since the last right one
static uint64_t not_before;             // no check starts before this time (ms)
static char pending[LOGIN_PASSWORD_MAX + 1];    // the answer, from typed until its check starts
static bool have_pending, checking;
static uint64_t last_activity;          // when something was last typed (ms)
static bool was_connected;
static bool dropping;                   // the terminal went away and hasn't come back yet ...
static uint64_t drop_since;             // ... since this time (ms)

static void wipe(void *p, size_t len) {
    volatile char *v = p;
    while (len--)
        *v++ = 0;
}

// After the nth wrong answer in a row: 1 s, 2 s, 4 s ... up to 30 s. With the hooks for it the
// count is kept across reboots, so a reset doesn't shorten the wait. Whoever can rewrite the
// flash can still reset a count kept there; every check costs its slow derivation regardless.
static uint32_t delay_ms(unsigned n) {
    if (n == 0)
        return 0;
    return n > 5 ? 30000 : 1000u << (n - 1);
}

static void ask(shell_t *sh);

// An answer is counted when its check starts, not when it turns out wrong: if the board is
// reset in between, the count already has it.
static void count_attempt(void) {
    failures++;
    if (hooks->count_attempt)
        hooks->count_attempt();
}

static void wrong(shell_t *sh) {
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
        if (checking) {
            // Ctrl-C during the check: the answer stays counted and earns its delay, like a
            // wrong one. Otherwise interrupting every check would be a way round the count.
            hooks->check_abort();
            not_before = hooks->now_ms() + delay_ms(failures);
        }
        checking = false;
        have_pending = false;
        wipe(pending, sizeof(pending));
        ask(sh);
        return false;
    }
    if (!checking) {
        if (hooks->now_ms() < not_before)
            return true;
        count_attempt();
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
    if (hooks->clear_count)
        hooks->clear_count();
    last_activity = hooks->now_ms();
    shell_clear_history(sh);            // nothing typed while locked is offered by Up
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
    // What was counted before this boot still counts, and so does the wait it earned.
    failures = hooks->kept_failures ? hooks->kept_failures() : 0;
    not_before = hooks->now_ms() + delay_ms(failures);
    have_pending = checking = false;
    last_activity = hooks->now_ms();
    was_connected = false;
    dropping = false;
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

void login_password_set(void) {
    failures = 0;
    not_before = 0;
    if (hooks && hooks->clear_count)
        hooks->clear_count();
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
    if (!locked)
        shell_printf(console, "console locked\n");
    locked = true;
    shell_clear_history(console);       // the next person to log in doesn't see these commands
    ask(console);
}

void login_activity(void) {
    if (hooks)
        last_activity = hooks->now_ms();
}

// The console locks itself: end whatever is running, throw away what was typed, ask again.
static void auto_lock(shell_t *console, const char *why) {
    shell_end(console);                 // a running job is cancelled (it frees what it holds)
    shell_discard_input(console);
    locked = true;
    if (hooks->auto_locked)
        hooks->auto_locked(why);
    shell_hide_input(console);
    shell_printf(console, "console locked (%s)\n", why);
    ask(console);
    shell_show_input(console);
}

// A drop locks the console only once it has lasted LOGIN_DISCONNECT_MS: the USB line state can
// read "disconnected" for a moment around a host closing and reopening the port, and a single
// reading used to lock a login made just then (#153), so the next line typed was taken as a
// password. Drops while locked don't count.
void login_poll(shell_t *console, bool connected, uint32_t idle_limit_ms) {
    bool dropped = was_connected && !connected;
    was_connected = connected;
    if (!hooks || locked || !hooks->has_password() || hooks->no_keys()) {
        dropping = false;
        return;
    }
    uint64_t now = hooks->now_ms();
    if (connected) {
        dropping = false;
    } else if (dropped) {
        dropping = true;
        drop_since = now;
    }
    if (dropping && now - drop_since >= LOGIN_DISCONNECT_MS) {
        dropping = false;
        auto_lock(console, "the terminal was disconnected");
    } else if (idle_limit_ms && now - last_activity >= idle_limit_ms) {
        auto_lock(console, "idle");
    }
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
