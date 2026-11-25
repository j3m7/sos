// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Messages between the people logged in: `wall` (to every other shell), `chat` (a room) and
// `who`. Pure C, host tested; the platform supplies the time through chat_hooks_t.
//
// Every live shell is registered as a peer. Posting a message only appends it to a small ring;
// each shell picks up what's new for it in chat_deliver(), called right after its shell_poll(),
// writing only what its channel takes without waiting. So a slow or stalled client can't hold
// up the sender or anyone else: it falls behind, and if the ring overtakes it, it's told how
// many messages it missed.

#ifndef SOS_CHAT_H
#define SOS_CHAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "shell/shell.h"

#ifndef CHAT_MAX_PEERS
#define CHAT_MAX_PEERS 4        // the console and SOS_SSH_MAX_SESSIONS (3) SSH sessions
#endif
#define CHAT_RING 16            // messages kept
#define CHAT_HISTORY 5          // chat lines shown on joining
#define CHAT_NAME_MAX 47        // display name, including a "/<slot>" suffix
#define CHAT_FROM_MAX 47        // where a peer is connected from, e.g. "ssh 192.168.1.20"
#define CHAT_STAMP_MAX 11       // "14:03", or "+12d03h" before the clock is set

typedef struct {
    uint64_t (*now_us)(void);                   // monotonic time, for `who`
    void (*stamp)(char *buf, size_t len);       // the time to show on a message
} chat_hooks_t;

void chat_init(const chat_hooks_t *hooks);

// A shell has started (after shell_init): `slot` is shown by `who` (0 = console, SSH sessions
// from 1), `name` is who it is (a key comment; filtered for the terminal, shortened, and given
// a "/<slot>" suffix if another peer already has that name), `from` where it's connected from.
// False if the table is full.
bool chat_register(shell_t *sh, unsigned slot, const char *name, const char *from);
// The shell's session has ended (call it next to shell_end): leaves the room, announcing it.
void chat_unregister(shell_t *sh);

// Print what's new for this shell, as much as its channel takes now. Call after shell_poll().
void chat_deliver(shell_t *sh);

// The commands; `args` is NULL without arguments (as shell_execute() splits them).
void chat_cmd_wall(shell_t *sh, char *args);
void chat_cmd_chat(shell_t *sh, char *args);
void chat_cmd_who(shell_t *sh, char *args);

// For tests: the peer's display name, or NULL if not registered.
const char *chat_name(const shell_t *sh);

#endif
