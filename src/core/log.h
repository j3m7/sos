// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// System log. Every message is stored in a RAM history (shown by `dmesg` in any shell) and, if
// important enough, printed on the console. Console printing hides and redraws the console's
// input line around log lines (through hooks), so logs never mangle what's being typed.
//
// Messages logged in interrupt context are only stored; log_flush(), called from the main loop,
// prints them. Platform access goes through log_hooks_t, so the module runs in host unit tests.
//
// Each message has a facility, OR-ed into the priority as in syslog:
// log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "console: logged in"). The facility says which log a
// message belongs to (#104): kern, auth (logins and every configuration change), daemon (network
// and background services) or user (scripts and `logger`). A message without one (facility 0) is
// a caller still to be tagged; it ends up in the last-resort log, never in several.

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
#define SOS_LOG_LEVEL_MASK 0x07

// Facilities (bits 3-6 of the priority). Not syslog's numbers: 0 is "none", so an untagged
// message is told apart from a kern one.
#define SOS_LOG_NONE    (0 << 3)
#define SOS_LOG_KERN    (1 << 3)    // boot, resets, faults, glitch detector, root secret, memory
#define SOS_LOG_AUTH    (2 << 3)    // logins (console and SSH), passwords, configuration changes
#define SOS_LOG_DAEMON  (3 << 3)    // Wi-Fi, SSH server, time, HTTP/TLS, SD card, chat
#define SOS_LOG_USER    (4 << 3)    // Lua scripts, `logger`
#define SOS_LOG_FACILITY_MASK 0x78
#define SOS_LOG_FACILITIES 5        // facility numbers 0 .. SOS_LOG_FACILITIES - 1

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

// `priority`: a facility OR-ed with a level. Levels below SOS_LOG_ERR (syslog's emerg, alert,
// crit) count as errors.
void log_msg(int priority, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void log_vmsg(int priority, const char *fmt, va_list ap) __attribute__((format(printf, 2, 0)));
// The same with `prefix` (plain text, not a format) put before the message, so a caller that
// tags someone else's messages needs no buffer or formatting pass of its own.
void log_vmsg_prefixed(int priority, const char *prefix, const char *fmt, va_list ap)
    __attribute__((format(printf, 3, 0)));

// Print stored messages the console hasn't shown yet. Thread context; call from the main loop.
// Does nothing while hooks->hold() is true.
void log_flush(void);

// How many stored messages the console will print at the next log_flush() (one more if some
// were dropped before it showed them). For a full-screen program to say messages are waiting.
int log_pending(void);

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

// "none", "kern", "auth", "daemon", "user" for a facility (SOS_LOG_AUTH etc., level bits ignored);
// or NULL. Parsing gives the facility value, or -1 if unknown.
const char *log_facility_name(int facility);
int log_facility_parse(const char *name);

// Iterate over the history, oldest first. Start with *cursor = 0; each call copies the next
// message (with timestamp, without the trailing newline), sets *priority (facility | level) if
// it isn't NULL, and returns true, or returns false when there are no more. Messages overwritten
// since the previous call are skipped.
bool log_history_next(uint32_t *cursor, char *buf, size_t buflen, int *priority);

// For the log files (core/logfile.c): the entries they haven't taken yet, oldest first. Each call
// takes the next one into *e: its priority, its time since boot (ms), and its text without the
// time and the level's tag. False when there are none. Entries are kept in the history for
// `dmesg` either way.
typedef struct {
    int priority;               // facility | level
    uint64_t uptime_ms;
    char text[LOG_LINE_MAX + 1];
} log_entry_t;

bool log_file_next(log_entry_t *e);

// The history entry after *cursor, as log_history_next() but split like log_file_next()'s (for
// Lua's sos.dmesg). *cursor = 0 starts at the oldest; a cursor past the newest entry starts after
// it. *lost (if not NULL) is set when entries after *cursor were overwritten before this call:
// the cursor then jumps to the oldest kept.
bool log_history_entry(uint32_t *cursor, log_entry_t *e, bool *lost);
// How many entries were dropped from the history before the log files took them, since the
// last call (which resets the count).
unsigned log_file_lost(void);

// A rate limit for messages from outside the firmware (scripts, `logger`), so a loop can't push
// everything else (failed logins, warnings) out of the history: at most `per_sec` messages in
// each second. Zero-initialize; one per source.
typedef struct {
    uint64_t window_us;         // start of the current second
    unsigned count;             // messages let through in it
    unsigned dropped;           // refused since log_quota_dropped() last took the count
} log_quota_t;

// True if one more message may go in now; false (and counted as dropped) if not.
bool log_quota_take(log_quota_t *q, unsigned per_sec);
// The number dropped since the last call, and resets it: report it once a message goes through.
unsigned log_quota_dropped(log_quota_t *q);

#endif
