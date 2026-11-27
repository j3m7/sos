// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "core/log.h"

#include <stdio.h>
#include <string.h>

// History: a byte ring of entries "<priority byte><text>\n", where the priority byte is 0x80 |
// facility | level. Text is printable ASCII only, so that byte is never '\n' or text. Positions
// are absolute byte counts; the byte at position p lives at hist[p % LOG_HISTORY_SIZE].
// start..end is what's stored; printed..end is what the console hasn't shown yet; written..end
// what the log files (core/logfile.c) haven't taken yet.

static char hist[LOG_HISTORY_SIZE];
static uint32_t start, end, printed, written;
static bool lost_unprinted;             // entries dropped before the console showed them
static unsigned lost_unwritten;         // entries dropped before the log files took them
// Entries the console hasn't been offered yet (from `printed` on), by level, so log_pending()
// doesn't walk the history with the lock held. Kept in step by append(), drop_oldest() and
// log_flush(). Levels are syslog's, 0 to 7.
#define LOG_LEVELS 8
static uint16_t unprinted[LOG_LEVELS];
static bool flushing;
static bool deferred;
static int console_level = SOS_LOG_NOTICE;
static int history_level = SOS_LOG_INFO;
static const log_hooks_t *hooks;

static bool before(uint32_t a, uint32_t b) {    // a < b, with wrap-around
    return (int32_t)(a - b) < 0;
}

static char at(uint32_t pos) {
    return hist[pos % LOG_HISTORY_SIZE];
}

static unsigned level_slot(int level) {
    return level < 0 ? 0 : level >= LOG_LEVELS ? LOG_LEVELS - 1 : (unsigned)level;
}

void log_init(const log_hooks_t *h) {
    hooks = h;
    start = end = printed = written = 0;
    lost_unprinted = false;
    lost_unwritten = 0;
    memset(unprinted, 0, sizeof(unprinted));
    deferred = false;
}

void log_set_deferred(bool d) { deferred = d; }
void log_set_console_level(int level) { console_level = level; }
int log_console_level(void) { return console_level; }
void log_set_history_level(int level) { history_level = level; }
int log_history_level(void) { return history_level; }

// Levels of a priority; emerg, alert and crit count as err (the level byte has room for them,
// but nothing here tells them apart).
static int level_of(int priority) {
    int level = priority & SOS_LOG_LEVEL_MASK;
    return level < SOS_LOG_ERR ? SOS_LOG_ERR : level;
}

static const char *const level_names[] = {
    [SOS_LOG_ERR] = "err", [SOS_LOG_WARNING] = "warning", [SOS_LOG_NOTICE] = "notice",
    [SOS_LOG_INFO] = "info", [SOS_LOG_DEBUG] = "debug",
};

const char *log_level_name(int level) {
    if (level < SOS_LOG_ERR || level > SOS_LOG_DEBUG)
        return NULL;
    return level_names[level];
}

int log_level_parse(const char *name) {
    for (int l = SOS_LOG_ERR; l <= SOS_LOG_DEBUG; l++)
        if (!strcmp(name, level_names[l]))
            return l;
    return -1;
}

static const char *const facility_names[SOS_LOG_FACILITIES] = {
    "none", "kern", "auth", "daemon", "user",
};

const char *log_facility_name(int facility) {
    unsigned f = (unsigned)(facility & SOS_LOG_FACILITY_MASK) >> 3;
    return f < SOS_LOG_FACILITIES ? facility_names[f] : NULL;
}

int log_facility_parse(const char *name) {
    for (int f = 0; f < SOS_LOG_FACILITIES; f++)
        if (!strcmp(name, facility_names[f]))
            return f << 3;
    return -1;
}

// The byte that starts an entry, and back.
static char entry_byte(int facility, int level) {
    return (char)(0x80 | (facility & SOS_LOG_FACILITY_MASK) | level);
}

static int entry_priority(char c) {
    return (unsigned char)c & 0x7f;
}

// Drop the oldest entry. Caller holds the lock.
static void drop_oldest(void) {
    if (before(start, end) && !before(start, printed))  // at its level byte; not shown yet
        unprinted[level_slot(entry_priority(at(start)) & SOS_LOG_LEVEL_MASK)]--;
    if (before(start, end) && !before(start, written))  // not taken for the files yet
        lost_unwritten++;
    while (before(start, end) && at(start) != '\n')
        start++;
    if (before(start, end))
        start++;                                // past the '\n'
    if (before(printed, start)) {
        printed = start;
        lost_unprinted = true;
    }
    if (before(written, start))
        written = start;
}

// Store one entry. Caller holds the lock.
static void append(int facility, int level, const char *text, size_t n) {
    uint32_t need = (uint32_t)n + 2;
    while (end - start + need > LOG_HISTORY_SIZE)
        drop_oldest();
    hist[end++ % LOG_HISTORY_SIZE] = entry_byte(facility, level);
    unprinted[level_slot(level)]++;
    for (size_t i = 0; i < n; i++)
        hist[end++ % LOG_HISTORY_SIZE] = text[i];
    hist[end++ % LOG_HISTORY_SIZE] = '\n';
}

void log_vmsg(int priority, const char *fmt, va_list ap) {
    log_vmsg_prefixed(priority, "", fmt, ap);
}

// May run in interrupt context: a message that won't be kept costs only the level check, and
// one that will is formatted once, into the one buffer here.
void log_vmsg_prefixed(int priority, const char *prefix, const char *fmt, va_list ap) {
    char line[LOG_LINE_MAX + 1];
    int level = level_of(priority);
    if (!hooks || level > history_level)
        return;

    uint64_t us = hooks->now_us();
    const char *tag = level <= SOS_LOG_ERR ? "error: " : level == SOS_LOG_WARNING ? "warning: " : "";
    int n = snprintf(line, sizeof(line), "[%5lu.%03lu] %s%s",
                     (unsigned long)(us / 1000000), (unsigned long)(us / 1000 % 1000), tag,
                     prefix);
    if (n < 0)
        return;
    if ((size_t)n < sizeof(line))
        vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    size_t len = strlen(line);
    while (len > 0 && line[len - 1] == '\n')    // one message, one line
        line[--len] = '\0';
    // Messages quote text from outside (an SSH client's user name, file names): only printable
    // ASCII goes through, so it can't send escape sequences to a terminal, fake a log line with
    // a carriage return, or break the ring's framing.
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)line[i];
        if (c < 0x20)
            line[i] = ' ';
        else if (c > 0x7e)
            line[i] = '?';
    }

    uint32_t saved = hooks->lock();
    append(priority & SOS_LOG_FACILITY_MASK, level, line, len);
    hooks->unlock(saved);

    if (!deferred && !hooks->in_irq())
        log_flush();
}

void log_msg(int priority, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vmsg(priority, fmt, ap);
    va_end(ap);
}

// Copy the entry at *pos (priority byte not included) and advance past it. Caller holds the
// lock. Returns the entry's priority (facility | level).
static int copy_entry(uint32_t *pos, char *buf, size_t buflen) {
    int priority = entry_priority(at(*pos));
    size_t n = 0;
    (*pos)++;
    while (before(*pos, end) && at(*pos) != '\n') {
        if (n + 1 < buflen)
            buf[n++] = at(*pos);
        (*pos)++;
    }
    if (before(*pos, end))
        (*pos)++;
    if (buflen)
        buf[n] = '\0';
    return priority;
}

void log_flush(void) {
    char line[LOG_LINE_MAX + 2];
    bool hidden = false;

    if (!hooks || flushing || hooks->in_irq() || (hooks->hold && hooks->hold()))
        return;
    flushing = true;
    for (;;) {
        uint32_t saved = hooks->lock();
        bool lost = lost_unprinted;
        lost_unprinted = false;
        bool more = before(printed, end);
        int level = more ? copy_entry(&printed, line, sizeof(line) - 1) & SOS_LOG_LEVEL_MASK : 0;
        if (more)
            unprinted[level_slot(level)]--;
        hooks->unlock(saved);

        if (!lost && !more)
            break;
        if (!lost && level > console_level)
            continue;
        if (!hidden) {
            hooks->hide_input();
            hidden = true;
        }
        if (lost) {
            static const char msg[] = "[log: some messages were dropped before they were shown]\n";
            hooks->write(msg, sizeof(msg) - 1);
        }
        if (more && level <= console_level) {
            size_t n = strlen(line);
            line[n++] = '\n';
            hooks->write(line, n);
        }
    }
    if (hidden)
        hooks->show_input();
    flushing = false;
}

int log_pending(void) {
    if (!hooks)
        return 0;
    uint32_t saved = hooks->lock();
    int n = lost_unprinted ? 1 : 0;
    for (int level = 0; level < LOG_LEVELS && level <= console_level; level++)
        n += unprinted[level];
    hooks->unlock(saved);
    return n;
}

bool log_history_next(uint32_t *cursor, char *buf, size_t buflen, int *priority) {
    if (!hooks)
        return false;
    uint32_t saved = hooks->lock();
    // 0 means "from the oldest"; positions overwritten since the last call jump forward too.
    if (*cursor == 0 || before(*cursor, start))
        *cursor = start;
    bool ok = before(*cursor, end);
    if (ok) {
        int p = copy_entry(cursor, buf, buflen);
        if (priority)
            *priority = p;
    }
    hooks->unlock(saved);
    return ok;
}

bool log_quota_take(log_quota_t *q, unsigned per_sec) {
    uint64_t now = hooks ? hooks->now_us() : 0;
    if (now - q->window_us >= 1000000) {
        q->window_us = now;
        q->count = 0;
    }
    if (q->count < per_sec) {
        q->count++;
        return true;
    }
    q->dropped++;
    return false;
}

unsigned log_quota_dropped(log_quota_t *q) {
    unsigned n = q->dropped;
    q->dropped = 0;
    return n;
}

// The parts of a stored line "[    1.234] warning: text": the time since boot, and the text
// after the time and the level's tag (the level is in the priority).
static void split_line(const char *line, int level, log_entry_t *e) {
    const char *p = line;
    uint64_t secs = 0, ms = 0;
    if (*p == '[') {
        p++;
        while (*p == ' ')
            p++;
        while (*p >= '0' && *p <= '9')
            secs = secs * 10 + (uint64_t)(*p++ - '0');
        if (*p == '.')
            p++;
        for (int i = 0; i < 3 && *p >= '0' && *p <= '9'; i++)
            ms = ms * 10 + (uint64_t)(*p++ - '0');
        if (*p == ']')
            p++;
        if (*p == ' ')
            p++;
    }
    e->uptime_ms = secs * 1000 + ms;
    const char *tag = level <= SOS_LOG_ERR       ? "error: "
                      : level == SOS_LOG_WARNING ? "warning: "
                                                 : "";
    if (*tag && !strncmp(p, tag, strlen(tag)))
        p += strlen(tag);
    memmove(e->text, p, strlen(p) + 1);
}

bool log_file_next(log_entry_t *e) {
    if (!hooks)
        return false;
    uint32_t saved = hooks->lock();
    if (before(written, start))
        written = start;
    bool ok = before(written, end);
    if (ok)
        e->priority = copy_entry(&written, e->text, sizeof(e->text));
    hooks->unlock(saved);
    if (ok)
        split_line(e->text, e->priority & SOS_LOG_LEVEL_MASK, e);
    return ok;
}

bool log_history_entry(uint32_t *cursor, log_entry_t *e, bool *lost) {
    if (lost)
        *lost = false;
    if (!hooks)
        return false;
    uint32_t saved = hooks->lock();
    if (*cursor == 0 || before(*cursor, start)) {
        if (*cursor != 0 && lost)
            *lost = true;                       // the entries after it were overwritten
        *cursor = start;
    } else if (before(end, *cursor)) {
        *cursor = end;                          // past the newest: not a cursor this log gave
    }
    bool ok = before(*cursor, end);
    if (ok)
        e->priority = copy_entry(cursor, e->text, sizeof(e->text));
    hooks->unlock(saved);
    if (ok)
        split_line(e->text, e->priority & SOS_LOG_LEVEL_MASK, e);
    return ok;
}

unsigned log_file_lost(void) {
    if (!hooks)
        return 0;
    uint32_t saved = hooks->lock();
    unsigned n = lost_unwritten;
    lost_unwritten = 0;
    hooks->unlock(saved);
    return n;
}
