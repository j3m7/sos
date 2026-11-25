// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// System log. Every message is stored in a RAM history (shown by `dmesg` in any shell) and, if
// important enough, printed on the console. Console printing hides and redraws the console's
// input line around log lines (through hooks), so logs never mangle what's being typed.
//
// Messages logged in interrupt context are only stored; log_flush(), called from the main loop,
// prints them. Platform access goes through log_hooks_t, so the module runs in host unit tests.

#ifndef SOS_LOG_H
#define SOS_LOG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Levels as in syslog (and pico-sshd's log.h).
#define SOS_LOG_ERR     3
#define SOS_LOG_WARNING 4
#define SOS_LOG_NOTICE  5
#define SOS_LOG_INFO    6
#define SOS_LOG_DEBUG   7

#define LOG_HISTORY_SIZE 4096   // bytes of RAM history (oldest messages are dropped first)
#define LOG_LINE_MAX     160    // longer messages are truncated

typedef struct {
    uint64_t (*now_us)(void);                   // time since boot
    bool (*in_irq)(void);                       // true in interrupt context
    uint32_t (*lock)(void);                     // exclusive access (e.g. disable interrupts)
    void (*unlock)(uint32_t saved);
    void (*write)(const char *s, size_t n);     // console output (thread context only)
    void (*hide_input)(void);                   // before printing: clear the input line
    void (*show_input)(void);                   // after printing: redraw prompt and input
    // True while the console mustn't be written to (a full-screen program runs there): messages
    // are kept and printed by the first log_flush() after it returns false. NULL: never.
    bool (*hold)(void);
} log_hooks_t;

void log_init(const log_hooks_t *hooks);

void log_msg(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void log_vmsg(int level, const char *fmt, va_list ap);

// Print stored messages the console hasn't shown yet. Thread context; call from the main loop.
// Does nothing while hooks->hold() is true.
void log_flush(void);

// Deferred mode: messages are only stored when logged and printed by the next log_flush(), so
// they never land in the middle of a shell command's output. Off by default (boot messages print
// immediately); turn it on once the main loop calls log_flush() between commands.
void log_set_deferred(bool deferred);

// Messages above these levels are not printed / not stored.
void log_set_console_level(int level);
int log_console_level(void);
void log_set_history_level(int level);
int log_history_level(void);

// "err", "warning", "notice", "info", "debug"; or NULL.
const char *log_level_name(int level);
int log_level_parse(const char *name);          // -1 if unknown

// Iterate over the history, oldest first. Start with *cursor = 0; each call copies the next
// message (with timestamp, without the trailing newline) and returns true, or returns false when
// there are no more. Messages overwritten since the previous call are skipped.
bool log_history_next(uint32_t *cursor, char *buf, size_t buflen);

#endif
