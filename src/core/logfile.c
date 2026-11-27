// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "core/logfile.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "core/log.h"

static const char *const paths[SOS_LOG_FACILITIES] = {
    LOGFILE_DIR "/syslog", LOGFILE_DIR "/kern.log", LOGFILE_DIR "/auth.log",
    LOGFILE_DIR "/daemon.log", LOGFILE_DIR "/user.log",
};

typedef struct {
    char data[LOGFILE_BUF];
    size_t len;
    uint64_t first_us;          // when its oldest line came in
    bool urgent;                // holds a warning or an error: write it out now
    bool started;               // the boot marker has gone in this boot
} buffer_t;

static const logfile_hooks_t *hooks;
static buffer_t bufs[SOS_LOG_FACILITIES];
static logfile_state_t state;
static int file_level = SOS_LOG_INFO;
static uint32_t stopped_generation;
static const char *error, *error_path;
static uint32_t bytes, lines, lost;
static uint64_t last_retry_us;

void logfile_init(const logfile_hooks_t *h) {
    hooks = h;
    memset(bufs, 0, sizeof(bufs));
    state = LOGFILE_NO_CARD;
    error = error_path = NULL;
    bytes = lines = lost = 0;
    last_retry_us = h ? h->now_us() : 0;    // logfile_start() has just tried
}

static void take(void);

// What's waiting is taken under the old level first: a change is logged just before it's made,
// and `log file err` mustn't filter out its own record.
void logfile_set_level(int level) {
    if (hooks)
        take();
    file_level = level;
}

int logfile_level(void) { return file_level; }

const char *logfile_path(int facility) {
    unsigned f = (unsigned)(facility & SOS_LOG_FACILITY_MASK) >> 3;
    return f < SOS_LOG_FACILITIES ? paths[f] : NULL;
}

void logfile_status(logfile_status_t *st) {
    st->state = hooks ? state : LOGFILE_OFF;
    st->error = error;
    st->error_path = error_path;
    st->bytes = bytes;
    st->lines = lines;
    st->lost = lost;
    st->buffered = 0;
    for (int f = 0; f < SOS_LOG_FACILITIES; f++)
        st->buffered += bufs[f].len;
}

// "2026-11-26T14:03:12.345Z" for a message logged `uptime_ms` after boot, or "+12.345" (since
// boot) while the clock isn't set.
static void stamp(char *out, size_t len, uint64_t uptime_ms) {
    int64_t wall;
    uint64_t now = hooks->now_us();
    if (hooks->wall_us(&wall)) {
        uint64_t age_us = now > uptime_ms * 1000 ? now - uptime_ms * 1000 : 0;
        int64_t t_ms = (wall - (int64_t)age_us) / 1000;
        time_t secs = (time_t)(t_ms / 1000);
        struct tm tm;
        gmtime_r(&secs, &tm);
        snprintf(out, len, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                 tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(t_ms % 1000));
    } else {
        snprintf(out, len, "+%llu.%03llu", (unsigned long long)(uptime_ms / 1000),
                 (unsigned long long)(uptime_ms % 1000));
    }
}

// Add a line to a facility's buffer (the caller made sure it fits).
static void put(int f, const char *line, size_t n) {
    buffer_t *b = &bufs[f];
    if (!b->len)
        b->first_us = hooks->now_us();
    memcpy(b->data + b->len, line, n);
    b->len += n;
}

static void put_line(int f, uint64_t uptime_ms, const char *level, const char *text) {
    char line[LOGFILE_LINE_MAX];
    char when[32];
    if (!bufs[f].started) {
        char boot[96];
        hooks->boot_text(boot, sizeof(boot));
        stamp(when, sizeof(when), 0);       // the boot's own time
        int n = snprintf(line, sizeof(line), "%s --- boot: %s ---\n", when, boot);
        put(f, line, n < (int)sizeof(line) ? (size_t)n : sizeof(line) - 1);
        bufs[f].started = true;
    }
    stamp(when, sizeof(when), uptime_ms);
    int n = snprintf(line, sizeof(line), "%s %s %s\n", when, level, text);
    if (n >= (int)sizeof(line)) {           // cut, keeping the newline
        n = (int)sizeof(line) - 1;
        line[n - 1] = '\n';
    }
    put(f, line, (size_t)n);
    lines++;
}

// Room the next line may need: the longest line, and the boot marker if it hasn't gone in yet.
static bool full(const buffer_t *b) {
    size_t need = b->started ? LOGFILE_LINE_MAX : 2 * LOGFILE_LINE_MAX;
    return LOGFILE_BUF - b->len < need;
}

// Room in every buffer for the next line, whichever file it's for: one more entry can be taken.
static bool room_everywhere(void) {
    for (int f = 0; f < SOS_LOG_FACILITIES; f++)
        if (full(&bufs[f]))
            return false;
    return true;
}

// Write a buffer out. False if the write failed (the writer has stopped).
static bool flush(int f) {
    buffer_t *b = &bufs[f];
    const char *why = "write failed";
    if (!hooks->append(paths[f], b->data, b->len, &why)) {
        state = LOGFILE_STOPPED;
        stopped_generation = hooks->card_generation();
        error = why;
        error_path = paths[f];
        // Into the history only: the writer is stopped, so this can't loop. The files get it
        // after the next mount.
        log_msg(SOS_LOG_DAEMON | SOS_LOG_ERR,
                "log: writing %s failed (%s); log files stopped until the card is mounted again",
                paths[f], why);
        return false;
    }
    bytes += (uint32_t)b->len;
    b->len = 0;
    b->urgent = false;
    return true;
}

static bool due(const buffer_t *b, uint64_t now) {
    return b->len && (b->urgent || full(b) ||
                      now - b->first_us >= (uint64_t)LOGFILE_FLUSH_MS * 1000);
}

void logfile_poll(void) {
    if (!hooks)
        return;
    if (state == LOGFILE_STOPPED) {
        if (hooks->card_generation() == stopped_generation)
            return;
        error = error_path = NULL;          // mounted again since: try again
    }
    if (!hooks->card_ready()) {
        uint64_t now = hooks->now_us();
        if (hooks->retry_mount && now - last_retry_us >= (uint64_t)LOGFILE_RETRY_MS * 1000) {
            last_retry_us = now;
            hooks->retry_mount();
        }
        if (!hooks->card_ready()) {
            state = LOGFILE_NO_CARD;
            return;
        }
    }
    state = LOGFILE_WRITING;
    take();

    // Write out what's due, a few files per pass.
    uint64_t now = hooks->now_us();
    for (int f = 0, done = 0; f < SOS_LOG_FACILITIES && done < LOGFILE_FLUSHES_PER_POLL; f++) {
        if (!due(&bufs[f], now))
            continue;
        if (!flush(f))
            return;
        done++;
    }
}

// Take what the history has into the buffers, while every buffer has room for one more line.
static void take(void) {
    log_entry_t e;
    while (room_everywhere()) {
        unsigned dropped = log_file_lost();
        if (dropped) {
            char text[80];
            snprintf(text, sizeof(text), "[log: %u message%s lost before they were written]",
                     dropped, dropped == 1 ? "" : "s");
            for (int f = 0; f < SOS_LOG_FACILITIES; f++)
                put_line(f, hooks->now_us() / 1000, "warning", text);
            lost += dropped;
            continue;
        }
        if (!log_file_next(&e))
            break;
        int level = e.priority & SOS_LOG_LEVEL_MASK;
        if (level > file_level)
            continue;
        int f = (e.priority & SOS_LOG_FACILITY_MASK) >> 3;
        if (f >= SOS_LOG_FACILITIES)
            f = 0;
        put_line(f, e.uptime_ms, log_level_name(level), e.text);
        if (level <= SOS_LOG_WARNING)
            bufs[f].urgent = true;
    }
}
