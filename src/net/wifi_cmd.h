// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

// The `wifi` and `net` commands (shell/shell.c lists them).

#ifndef SOS_WIFI_CMD_H
#define SOS_WIFI_CMD_H

#include "shell/shell.h"

void wifi_command(shell_t *sh, char *args);     // `wifi ...`
void net_command(shell_t *sh, char *args);      // `net`: Wi-Fi and IP status
extern const shell_word_t wifi_words[];          // what Tab offers after `wifi`

// The Wi-Fi settings and state, for `wifi` and `config`.
void wifi_show(shell_t *sh);

// Ask for an SSID, with Tab offering the networks around (`wifi set|add`, `setup`). The
// answer's callback calls wifi_names_stop().
void wifi_ask_ssid(shell_t *sh, const char *prompt, shell_answer_fn fn, void *arg);

#endif
