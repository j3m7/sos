// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Log files on the SD card (#104): every message the log keeps also goes to a file in /var/log
// picked by its facility, so the log survives a reboot:
//
//   kern     /var/log/kern.log         boot, resets, faults, glitch detector, root secret
//   auth     /var/log/auth.log         logins, passwords, every configuration change
//   daemon   /var/log/daemon.log       Wi-Fi, SSH server, time, HTTPS, SD card, chat
//   user     /var/log/user.log         Lua scripts, `logger`
//   none     /var/log/syslog           messages with no facility: the last resort
//
// Each message goes to one file. Lines read "2026-11-26T14:03:12.345Z notice console: logged in"
// (UTC), or "+12.345 ..." (seconds since boot) while the clock isn't set. The first lines a file
// gets in a boot are preceded by a boot marker naming the firmware and the last reset.
//
// The writer works from the main loop: logfile_poll() takes new messages from the log's history
// into a small buffer per file and writes a buffer out (open, append, sync, close) when it's
// full, holds a warning or an error, or its oldest line is LOGFILE_FLUSH_MS old. No file stays
// open between passes, so unmounting or formatting the card is never blocked. With no card
// mounted, messages wait in the history; those it drops meanwhile are counted and the files say
// so. A mount is tried quietly every LOGFILE_RETRY_MS meanwhile (not after `sd unmount`), so a
// card put back in is written to without anyone typing a command. After a failed write the writer stops, says so once in the log, and starts again at the
// next mount. Platform access goes through hooks, so the module runs in host tests.

#ifndef SOS_LOGFILE_H
#define SOS_LOGFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOGFILE_DIR       "/var/log"
#define LOGFILE_BUF       1024      // bytes buffered per file
#define LOGFILE_FLUSH_MS  5000      // the longest a line waits in a buffer
#define LOGFILE_LINE_MAX  208       // time, level, a full log line, newline
#define LOGFILE_FLUSHES_PER_POLL 2  // file writes in one main-loop pass, at most
#define LOGFILE_RETRY_MS  60000     // with no card mounted, try mounting one this often

typedef struct {
    uint64_t (*now_us)(void);                       // time since boot
    bool (*wall_us)(int64_t *unix_us);              // UTC now, false if the clock isn't set
    bool (*card_ready)(void);                       // a card is mounted
    uint32_t (*card_generation)(void);              // changes at every mount
    // Append to a file, making the directories on its path if needed; on failure, false and
    // *why says what went wrong.
    bool (*append)(const char *path, const char *data, size_t n, const char **why);
    // The boot marker's text, e.g. "SOS 1234abcd5678, last reset: watchdog".
    void (*boot_text)(char *buf, size_t len);
    // Try to mount a card, quietly (called every LOGFILE_RETRY_MS while none is mounted).
    void (*retry_mount)(void);
} logfile_hooks_t;

typedef enum {
    LOGFILE_OFF,                // not started
    LOGFILE_NO_CARD,            // no card mounted: messages wait in the history
    LOGFILE_WRITING,
    LOGFILE_STOPPED,            // a write failed; starts again at the next mount
} logfile_state_t;

typedef struct {
    logfile_state_t state;
    const char *error;          // LOGFILE_STOPPED: what failed
    const char *error_path;
    uint32_t bytes;             // written this boot
    uint32_t lines;
    uint32_t lost;              // messages dropped from the history before they were written
    size_t buffered;            // bytes waiting in the buffers
} logfile_status_t;

void logfile_init(const logfile_hooks_t *hooks);
// On the board (logfile_board.c): logfile_init() with the SD card's hooks, and a first mount.
void logfile_start(void);
void logfile_poll(void);
void logfile_status(logfile_status_t *st);

// Messages above this level aren't written (default info). The history's level still applies
// first: a message it doesn't keep never reaches the files.
void logfile_set_level(int level);
int logfile_level(void);

// The file a facility's messages go to (SOS_LOG_AUTH etc.), or NULL.
const char *logfile_path(int facility);

#endif
