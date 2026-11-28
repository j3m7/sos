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
    bool started;               // the marker has gone in (this boot, mount or rotation)
    const char *marker;         // the next marker: "boot", "mounted" or "rotated"
} buffer_t;

static const logfile_hooks_t *hooks;
static buffer_t bufs[SOS_LOG_FACILITIES];
static logfile_state_t state;
static int file_level = SOS_LOG_INFO;
static uint32_t stopped_generation;
static const char *error, *error_path;
static uint32_t bytes, lines, lost;
static uint32_t seen_generation;        // the mount the files were last written on
static unsigned rotate_days = 7, rotate_minute;
static bool rotated_known;              // `rotated` was read from the card (or set)
static int64_t rotated;
static uint32_t rotated_generation;     // the mount it was read on
static uint64_t last_retry_us;

void logfile_init(const logfile_hooks_t *h) {
    hooks = h;
    memset(bufs, 0, sizeof(bufs));
    state = LOGFILE_NO_CARD;
    error = error_path = NULL;
    bytes = lines = lost = 0;
    rotated_known = false;
    seen_generation = 0;
    for (int f = 0; f < SOS_LOG_FACILITIES; f++)
        bufs[f].marker = "boot";
    last_retry_us = h ? h->now_us() : 0;    // logfile_start() has just tried
}

void logfile_set_rotation(unsigned days, unsigned minute) {
    rotate_days = days ? days : 1;
    rotate_minute = minute;
}

// When the next rotation is due: `rotate_days` days after the day of the last, at the time.
static int64_t next_rotation(void) {
    int64_t day = rotated - ((rotated % 86400) + 86400) % 86400;
    return day + (int64_t)rotate_days * 86400 + (int64_t)rotate_minute * 60;
}

static void take(void);
static const char *rotate(bool take_first);
static bool flush(int f);

// What's waiting is taken first, under whichever of the old and new levels lets more through:
// a change is logged just before it's made, and neither `log file err` (raising it) nor
// `log file info` after it (lowering it again, logged while it's still err) may filter out its
// own record.
void logfile_set_level(int level) {
    if (hooks) {
        if (level > file_level)
            file_level = level;
        take();
    }
    file_level = level;
}

void logfile_flush(void) {
    if (!hooks || state == LOGFILE_STOPPED || !hooks->card_ready())
        return;
    take();
    for (int f = 0; f < SOS_LOG_FACILITIES; f++)
        if (bufs[f].len && !flush(f))
            return;
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
    st->rotate_days = rotate_days;
    st->rotate_minute = rotate_minute;
    st->rotated_known = rotated_known;
    st->rotated = rotated;
    st->next_rotation = rotated_known ? next_rotation() : 0;
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
        // Each field bounded to its digits, so the compiler sees the text fits (it can't know
        // a struct tm's ranges); for any real date the result is the same.
        snprintf(out, len, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                 (unsigned)(tm.tm_year + 1900) % 10000u, (unsigned)(tm.tm_mon + 1) % 100u,
                 (unsigned)tm.tm_mday % 100u, (unsigned)tm.tm_hour % 100u,
                 (unsigned)tm.tm_min % 100u, (unsigned)tm.tm_sec % 100u,
                 (unsigned)(((t_ms % 1000) + 1000) % 1000));
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
        // The boot's own time; a mount's or rotation's marker, the time of the line it comes
        // before (lines logged while no card was in come after a mount, earlier than it).
        bool boot_marker = !strcmp(bufs[f].marker, "boot");
        stamp(when, sizeof(when), boot_marker ? 0 : uptime_ms);
        int n = boot_marker
                    ? snprintf(line, sizeof(line), "%s --- boot: %s ---\n", when, boot)
                    : snprintf(line, sizeof(line), "%s --- %s; this boot: %s ---\n", when,
                               !strcmp(bufs[f].marker, "rotated") ? "rotated" : "card mounted",
                               boot);
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

// A write, rename or the rotation time failed: stop until the next mount. Into the history only:
// the writer is stopped, so this can't loop. The files get it after the next mount.
static void stop(const char *what, const char *path, const char *why) {
    state = LOGFILE_STOPPED;
    stopped_generation = hooks->card_generation();
    error = why;
    error_path = path;
    log_msg(SOS_LOG_DAEMON | SOS_LOG_ERR,
            "log: %s %s failed (%s); log files stopped until the card is mounted again", what,
            path, why);
}

// Write a buffer out. False if the write failed (the writer has stopped).
static bool flush(int f) {
    buffer_t *b = &bufs[f];
    if (!b->len)
        return true;
    const char *why = "write failed";
    if (!hooks->append(paths[f], b->data, b->len, &why)) {
        stop("writing", paths[f], why);
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
        // Not within LOGFILE_RETRY_MS of any mount attempt (logfile.h, last_mount_us): never a
        // second blocking card init in the main-loop pass of another.
        uint64_t last = last_retry_us;
        if (hooks->last_mount_us && hooks->last_mount_us() > last)
            last = hooks->last_mount_us();
        if (hooks->retry_mount && now - last >= (uint64_t)LOGFILE_RETRY_MS * 1000) {
            last_retry_us = now;
            hooks->retry_mount();
        }
        if (!hooks->card_ready()) {
            state = LOGFILE_NO_CARD;
            return;
        }
    }
    state = LOGFILE_WRITING;

    // Another mount (perhaps another card): each file that already had its marker gets one
    // again, so every card's files say which boot their lines are from.
    uint32_t gen = hooks->card_generation();
    if (gen != seen_generation) {
        seen_generation = gen;
        for (int f = 0; f < SOS_LOG_FACILITIES; f++)
            if (bufs[f].started) {
                bufs[f].started = false;
                bufs[f].marker = "mounted";
            }
    }

    // Rotation: the last one's time is read once per mount (another card has its own); none
    // yet starts the count now. Due: rotate before taking anything more, so what's logged from
    // now on goes to the new files (what was taken before goes to the old ones).
    int64_t wall_us;
    if (rotated_known && rotated_generation != gen)
        rotated_known = false;
    if (!rotated_known && hooks->wall_us(&wall_us)) {
        rotated_generation = gen;
        rotated_known = hooks->load_rotated(&rotated);
        // Kept while the clock was wrong (more than a day ahead): start the count now instead
        // of waiting for that day.
        if (rotated_known && rotated > wall_us / 1000000 + 86400)
            rotated_known = false;
        if (!rotated_known) {
            const char *why = "write failed";
            rotated = wall_us / 1000000;
            if (!hooks->save_rotated(rotated, &why)) {
                stop("writing", LOGFILE_DIR "/.rotated", why);
                return;
            }
            rotated_known = true;
        }
    }
    if (rotated_known && hooks->wall_us(&wall_us) && wall_us / 1000000 >= next_rotation()) {
        if (rotate(false))
            return;
    }
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

// By hand: what's been logged until now belongs to the old files.
const char *logfile_rotate(void) {
    return rotate(true);
}

static const char *rotate(bool take_first) {
    if (!hooks)
        return "the log files aren't started";
    if (state == LOGFILE_STOPPED && hooks->card_generation() == stopped_generation)
        return error;
    if (!hooks->card_ready())
        return "no card mounted";
    // What's buffered belongs to the old files.
    if (take_first)
        take();
    for (int f = 0; f < SOS_LOG_FACILITIES; f++)
        if (!flush(f))
            return error;
    // A rename failing part-way leaves the files before it rotated and .rotated as it was, so
    // after the next mount the rotation is due again and those files are rotated a second time
    // (their fresh .1 replaced by an almost empty one). Only on a card failing mid-operation, and
    // at most one period's lines: accepted rather than tracking each file's rotation.
    for (int f = 0; f < SOS_LOG_FACILITIES; f++) {
        const char *why = "rename failed";
        if (!hooks->rotate_file(paths[f], &why)) {
            stop("rotating", paths[f], why);
            return error;
        }
        if (bufs[f].started) {              // the new file starts with a marker
            bufs[f].started = false;
            bufs[f].marker = "rotated";
        }                                   // (one not written this boot keeps its boot marker)
    }
    // The time is kept only with the clock set: without it the count goes on from the last.
    int64_t wall_us;
    if (hooks->wall_us(&wall_us)) {
        const char *why = "write failed";
        rotated = wall_us / 1000000;
        rotated_known = true;
        rotated_generation = hooks->card_generation();
        if (!hooks->save_rotated(rotated, &why)) {
            stop("writing", LOGFILE_DIR "/.rotated", why);
            return error;
        }
    }
    state = LOGFILE_WRITING;
    log_msg(SOS_LOG_DAEMON | SOS_LOG_NOTICE, "log: files rotated");
    return NULL;
}
