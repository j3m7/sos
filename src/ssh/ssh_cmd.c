// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

// The `key` and `ssh` commands (moved from shell/shell.c, docs/design/build-features.md §4.1).

#include "ssh/ssh_cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/config.h"
#include "core/log.h"
#include "core/settings.h"
#include "ssh/sshd.h"

// --- keys ----------------------------------------------------------------------------------------

static void print_key(shell_t *sh, unsigned index, const config_key_t *k) {
    char type[32], fp[64];
    sshd_fingerprint(k->blob, k->len, fp, sizeof(fp));
    shell_printf(sh, "%2u  %-20s %s  %s\n", index + 1, config_key_type(k, type, sizeof(type)), fp,
                 k->comment);
}

static void key_list(shell_t *sh) {
    const sos_config_t *c = settings();
    if (!c->key_count) {
        shell_printf(sh, "no trusted keys: SSH logins are not possible%s\n",
                     sh->io->admin ? " (add one with 'key add')" : "");
        return;
    }
    for (unsigned i = 0; i < c->key_count; i++)
        print_key(sh, i, &c->keys[i]);
}

// Parse and add one authorized_keys line. Returns true if the key was added.
bool key_add_line(shell_t *sh, const char *line) {
    config_key_t key;
    const char *err = config_key_parse(line, &key);
    if (!err)
        err = config_key_add(settings(), &key);
    if (err) {
        shell_printf(sh, "not added: %s\n", err);
        return false;
    }
    shell_save_settings(sh);
    shell_printf(sh, "trusted:\n");
    print_key(sh, settings()->key_count - 1, &key);
    log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE,
            "config: trusted key added (%s)", key.comment[0] ? key.comment : "no comment");
    return true;
}

static void on_key_line(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (answer && *answer)
        key_add_line(sh, answer);
    else if (answer)
        shell_printf(sh, "nothing added\n");
}

void key_command(shell_t *sh, char *args) {
    char *sub = args ? strtok(args, " ") : NULL;
    char *rest = sub ? strtok(NULL, "") : NULL;
    if (!sub || !strcmp(sub, "list")) {
        key_list(sh);
    } else if (!strcmp(sub, "add")) {
        if (!shell_require_admin(sh))
            return;
        if (rest && *rest)
            key_add_line(sh, rest);
        else
            shell_ask(sh, "Paste the public key (e.g. ~/.ssh/id_ed25519.pub): ", false, on_key_line, NULL);
    } else if (!strcmp(sub, "del")) {
        if (!shell_require_admin(sh))
            return;
        char *end;
        long n = rest ? strtol(rest, &end, 10) : 0;
        if (!rest || *end || n < 1 || !config_key_remove(settings(), (unsigned)(n - 1))) {
            shell_printf(sh, "usage: key del <n>, n as shown by 'key list'\n");
            return;
        }
        shell_save_settings(sh);
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: trusted key %ld removed", n);
        shell_printf(sh, "removed key %ld\n", n);
        if (!settings()->key_count)
            shell_printf(sh, "warning: no trusted keys left; SSH logins are no longer possible\n");
    } else {
        shell_printf(sh, "usage: key [list] | key add [line] | key del <n>\n");
    }
}

void ssh_command(shell_t *sh, char *args) { (void)args; sshd_status(sh); }

// `key del <n>`: the trusted keys' numbers, as `key list` shows them, with their comments.
static void tab_key_numbers(shell_t *sh, shell_value_fn fn, void *arg) {
    (void)sh;
    const sos_config_t *c = settings();
    char n[4];
    for (unsigned i = 0; i < c->key_count; i++) {
        snprintf(n, sizeof(n), "%u", i + 1);
        fn(n, c->keys[i].comment, arg);
    }
}

static const shell_values_t key_numbers = SHELL_VALUES_INIT(tab_key_numbers);

// The words Tab offers (shell.h).
#define END {0}
#define W(name) {name, NULL, 0}
const shell_word_t key_words[] = {
    W("list"), {"add", NULL, SHELL_ADMIN}, {"del", SHELL_VALUES_OF(key_numbers), SHELL_ADMIN},
    END};
#undef W
#undef END
