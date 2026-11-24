// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_SHELL_H
#define SOS_SHELL_H

#include <stdbool.h>
#include <stddef.h>

// Minimal line-oriented shell. Each instance has its own line buffer and talks to one channel
// (the USB/UART console, an SSH session, ...) through a shell_io_t, so sessions don't share
// input or output.

#define SHELL_LINE_MAX 80

typedef struct {
    const char *name;                                   // channel name, e.g. "console", "ssh"
    int (*read)(void *ctx, char *buf, int len);         // non-blocking; bytes read, 0 if none
    void (*write)(void *ctx, const char *buf, int len);
    void (*logout)(void *ctx);                          // end the session; NULL if not possible
    bool crlf;                                          // translate "\n" to "\r\n" on output
} shell_io_t;

typedef struct {
    const shell_io_t *io;
    void *ctx;
    char line[SHELL_LINE_MAX + 1];
    size_t line_len;
    bool last_was_cr;
} shell_t;

// Bind a shell to a channel and print the greeting and first prompt.
void shell_init(shell_t *sh, const shell_io_t *io, void *ctx);

// Process any input waiting on the channel. Never blocks; call regularly from the main loop.
void shell_poll(shell_t *sh);

// printf-style output to the shell's channel.
void shell_printf(shell_t *sh, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
