// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_SSHD_H
#define SOS_SSHD_H

#include <stdbool.h>
#include <stdint.h>

#include "shell/shell.h"

// Start the SSH server on port 22 with the given host key (DER), accepting up to
// SOS_SSH_MAX_SESSIONS concurrent sessions. Each session gets its own shell instance; sessions
// are not part of the global stdio console. Server messages go to the system log. Only
// public-key login with a trusted key (settings) is accepted, under any user name.
// Does nothing if already running.
bool sshd_start(const uint8_t *hostkey, uint32_t hostkey_len);

bool sshd_running(void);

// Stop serving: every session is ended and the listener closed. sshd_start() starts it again,
// with whatever host key it is then given. For when the host key in use must not be used any
// more (the board's owner secret was destroyed).
void sshd_stop(void);

// The host key's fingerprint ("SHA256:..."), once started.
const char *sshd_host_fingerprint(void);

// "SHA256:<base64>" fingerprint of an SSH wire-format public key, as ssh-keygen -l shows it.
void sshd_fingerprint(const uint8_t *blob, uint32_t len, char *out, size_t out_len);

// Serve the SSH session's shell: starts a fresh shell for each new login and processes its
// input. Never blocks; call regularly from the main loop.
void sshd_poll(void);

// Print host key fingerprint, server statistics and the active sessions to the given shell
// (the caller's own session is marked with '*').
void sshd_status(shell_t *sh);

// What `ssh` shows, as values (for Lua's sos.ssh.status()). False if the server isn't running.
typedef struct {
    const char *fingerprint;    // "SHA256:..."
    const char *key_type;       // "ssh-ed25519"
    unsigned sessions, max_sessions;
    uint32_t refused_busy, refused_penalty, dropped_stale, logins;
} sshd_info_t;
bool sshd_info(sshd_info_t *info);

// Session `i` (0 .. max_sessions - 1), if one is open there: who and from where, its state, and
// for how long. False for a free slot.
typedef struct {
    char login[40];             // room for pico-sshd's 32 characters
    char address[48];           // "192.168.1.79:50362"
    const char *state;
    bool sftp;
    uint32_t seconds;
} sshd_session_info_t;
bool sshd_session(unsigned i, sshd_session_info_t *info);

#endif
