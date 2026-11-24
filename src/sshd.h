// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_SSHD_H
#define SOS_SSHD_H

#include <stdbool.h>
#include <stdint.h>

#include "shell.h"

// Start the SSH server on port 22 with the given host key (DER), accepting up to
// SOS_SSH_MAX_SESSIONS concurrent sessions. Each session gets its own shell instance; sessions
// are not part of the global stdio console. Server log messages
// go to the console. Only public-key login for the user/key in local.cmake is accepted.
bool sshd_start(const uint8_t *hostkey, uint32_t hostkey_len);

// Serve the SSH session's shell: starts a fresh shell for each new login and processes its
// input. Never blocks; call regularly from the main loop.
void sshd_poll(void);

// Print host key fingerprint, server statistics and the active sessions to the given shell
// (the caller's own session is marked with '*').
void sshd_status(shell_t *sh);

#endif
