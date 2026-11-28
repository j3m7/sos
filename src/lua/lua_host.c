// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Core 0's side of Lua: the `lua` shell command, and the requests the script on core 1 makes
// (files on the SD card, log, time, HTTPS). Requests are answered from the main loop, one per
// pass, each a bounded amount of work, so the watchdog, SSH and the console keep going whatever
// the script does. The script's output goes to the shell that started it, as fast as that
// shell's channel takes it.

#include "lua/lua_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "hardware/sync.h"
#include "pico/stdlib.h"

#include "ff.h"
#include "lua.h"

#include "chat/chat.h"
#include "core/random.h"
#include "core/fault.h"
#include "core/heaptag.h"
#include "core/log.h"
#include "core/settings.h"
#include "core/wdog.h"
#include "files/files.h"
#include "files/path.h"
#include "files/sdcard.h"
#include "lua/lua_bridge.h"
#include "lua/lua_complete.h"
#include "lua/lua_core1.h"
#include "lua/lua_hash.h"
#include "lua/lua_health.h"
#include "lua/lua_net.h"
#include "net/http.h"
#include "shell/words.h"
#include "time/timeconv.h"
#include "time/timesync.h"

#define LUA_MEM_LIMIT (128 * 1024)      // heap a script may use

lua_job_t lua_job;
lua_beat_t lua_beat;
lua_req_t lua_req;
lua_out_t lua_out;

static shell_t *owner;                  // shell that started the script; NULL once it's gone
static char *owner_code;                // `lua -e` source, freed when the script ends
static bool background;                 // started with `lua -b` or at boot: output to the log
static char job_desc[48];               // what's running, for `lua ps` and its start and end
// What the job's own log lines are tagged with: the script's path, or "lua -e" for code given
// on the command line (its code would make a long, noisy prefix on every line).
static char job_tag[48];

// What a script puts in the log (sos.log, and a background script's output) goes in at most
// SCRIPT_LOG_PER_SEC lines a second; beyond that lines are counted and dropped, so a script
// logging in a loop doesn't push everything else (failed logins, warnings) out of the log
// history. Always the user facility, tagged with the script.
#define SCRIPT_LOG_PER_SEC 10
static log_quota_t script_quota;

static void script_log_dropped(void) {
    unsigned n = log_quota_dropped(&script_quota);
    if (n)
        log_msg(SOS_LOG_USER | SOS_LOG_INFO, "%s: ... %u lines dropped (more than %d a second)",
                job_tag, n, SCRIPT_LOG_PER_SEC);
}

// One line from the script, if the rate limit lets it through.
static void script_log(int level, const char *text) {
    if (!log_quota_take(&script_quota, SCRIPT_LOG_PER_SEC))
        return;
    script_log_dropped();
    log_msg(SOS_LOG_USER | level, "%s: %s", job_tag, text);
}
static uint64_t job_start_us;

// The script may still act: its shell is there, or it runs in the background. A script whose
// shell has gone has been stopped and changes nothing more.
static bool script_alive(void) {
    return owner || background;
}

// --- answering requests ----------------------------------------------------------------------

// Core 0's messages reach the script whole (#220). The longest are http_error()'s, bounded by
// HTTP_ERROR_MAX; the rest (FatFs, paths, "interrupted") are shorter.
_Static_assert(sizeof(lua_req.error) >= HTTP_ERROR_MAX, "lua_req.error must hold any HTTP error");

static bool req_error(const char *msg) {
    snprintf(lua_req.error, sizeof(lua_req.error), "%s", msg);
    lua_req.result = -1;
    return false;
}

// Normalize the request's path and make sure the card is there.
static bool req_path(char *out) {
    if (!path_resolve("/", lua_req.path, out, LUA_PATH_MAX))
        return req_error("path too long");
    if (!sdcard_mount(NULL))
        return req_error("no SD card");
    return true;
}

static bool req_may_write(const char *path) {
    // Scripts get SFTP's rights even when started on the console: the file may have come over
    // SFTP, and the console's extra rights (/certs) are for what the person there types, not for
    // code they run.
    if (!script_alive() || !files_write_allowed(NULL, path))
        return req_error("not allowed to change this path");
    return true;
}

static void serve_read(void) {
    char path[LUA_PATH_MAX];
    FIL f;
    UINT n = 0;
    if (!req_path(path))
        return;
    FRESULT fr = f_open(&f, path, FA_READ);
    if (fr == FR_OK) {
        fr = f_lseek(&f, lua_req.offset);
        if (fr == FR_OK)
            fr = f_read(&f, lua_req.data, LUA_REQ_DATA, &n);
        f_close(&f);
    }
    if (fr != FR_OK) {
        req_error(files_error(fr));
        if (fr == FR_NO_FILE || fr == FR_NO_PATH || fr == FR_INVALID_NAME)
            lua_req.result = LUA_REQ_NOT_FOUND;
        return;
    }
    lua_req.len = n;
    lua_req.result = n;
}

static void serve_write(void) {
    char path[LUA_PATH_MAX];
    FIL f;
    UINT n = 0;
    if (!req_path(path) || !req_may_write(path))
        return;
    BYTE mode = FA_WRITE | (lua_req.offset ? FA_OPEN_APPEND : FA_CREATE_ALWAYS);
    FRESULT fr = f_open(&f, path, mode);
    if (fr == FR_OK) {
        fr = f_write(&f, lua_req.data, lua_req.len, &n);
        FRESULT cr = f_close(&f);
        if (fr == FR_OK)
            fr = cr;
        if (fr == FR_OK && n != lua_req.len)
            fr = FR_DENIED;                     // card full
    }
    if (fr != FR_OK) {
        req_error(files_error(fr));
        return;
    }
    lua_req.result = n;
}

static void serve_list(void) {
    char path[LUA_PATH_MAX];
    DIR dir;
    FILINFO fi;
    if (!req_path(path))
        return;
    FRESULT fr = f_opendir(&dir, path);
    if (fr != FR_OK) {
        req_error(files_error(fr));
        return;
    }
    uint32_t skip = lua_req.offset, used = 0;
    int64_t count = 0;
    while ((fr = f_readdir(&dir, &fi)) == FR_OK && fi.fname[0]) {
        if (skip) {
            skip--;
            continue;
        }
        size_t need = 5 + strlen(fi.fname) + 1;
        if (used + need > LUA_REQ_DATA)
            break;                              // the rest in the next request
        uint32_t size = fi.fsize > 0xffffffffu ? 0xffffffffu : (uint32_t)fi.fsize;
        memcpy(lua_req.data + used, &size, 4);
        lua_req.data[used + 4] = (fi.fattrib & AM_DIR) ? 1 : 0;
        memcpy(lua_req.data + used + 5, fi.fname, need - 5);
        used += (uint32_t)need;
        count++;
    }
    f_closedir(&dir);
    if (fr != FR_OK) {
        req_error(files_error(fr));
        return;
    }
    lua_req.len = used;
    lua_req.result = count;
}

static void serve_change(FRESULT (*op)(const TCHAR *)) {
    char path[LUA_PATH_MAX];
    if (!req_path(path) || !req_may_write(path))
        return;
    FRESULT fr = op(path);
    if (fr != FR_OK)
        req_error(files_error(fr));
}

static FRESULT do_mkdir(const TCHAR *p) { return f_mkdir(p); }

// sos.rename: both paths must be ones a script may change. With offset 1, a file at `to` is
// removed first (not a directory), so the rename replaces it; there's a moment with only `from`.
static void serve_rename(void) {
    char from[LUA_PATH_MAX], to[LUA_PATH_MAX];
    lua_req.data[LUA_REQ_DATA - 1] = 0;
    if (!req_path(from) || !req_may_write(from))
        return;
    if (!path_resolve("/", (const char *)lua_req.data, to, sizeof(to))) {
        req_error("path too long");
        return;
    }
    if (!req_may_write(to))
        return;
    // One file under two spellings (another case): with replace, the unlink would delete it.
    if (!strcasecmp(from, to)) {
        req_error("source and destination are the same");
        return;
    }
    FILINFO fi;
    if (lua_req.offset && f_stat(to, &fi) == FR_OK) {
        if (fi.fattrib & AM_DIR) {
            req_error("a directory is in the way");
            return;
        }
        FRESULT fr = f_unlink(to);
        if (fr != FR_OK) {
            req_error(files_error(fr));
            return;
        }
    }
    FRESULT fr = f_rename(from, to);
    if (fr != FR_OK)
        req_error(files_error(fr));
}

// sos.stat: size, kind and time; LUA_REQ_NOT_FOUND if there's nothing at the path.
static void serve_stat(void) {
    char path[LUA_PATH_MAX];
    if (!req_path(path))
        return;
    lua_stat_t st = {0};
    if (!strcmp(path, "/")) {                   // f_stat can't stat the root
        st.dir = true;
    } else {
        FILINFO fi;
        FRESULT fr = f_stat(path, &fi);
        if (fr != FR_OK) {
            req_error(files_error(fr));
            if (fr == FR_NO_FILE || fr == FR_NO_PATH || fr == FR_INVALID_NAME)
                lua_req.result = LUA_REQ_NOT_FOUND;
            return;
        }
        st.size = fi.fsize;
        st.dir = (fi.fattrib & AM_DIR) != 0;
        if (fi.fdate)
            st.time = timeconv_from_tm(80 + (fi.fdate >> 9), ((fi.fdate >> 5) & 15) - 1,
                                       fi.fdate & 31, fi.ftime >> 11, (fi.ftime >> 5) & 63,
                                       (fi.ftime & 31) * 2);
    }
    memcpy(lua_req.data, &st, sizeof(st));
    lua_req.len = sizeof(st);
}
static FRESULT do_unlink(const TCHAR *p) { return f_unlink(p); }

// sos.reboot([why]). Refused in the first LUA_REBOOT_MIN_S after boot: a normal restart runs
// /autorun.lua again (only a crash or watchdog reset skips it), so a script that reboots at once
// would otherwise loop. A script whose shell has gone changes nothing more, this included.
static void serve_reboot(void) {
    uint64_t up_s = time_us_64() / 1000000;
    if (!script_alive()) {
        req_error("the script's session has ended");
        return;
    }
    if (up_s < LUA_REBOOT_MIN_S) {
        char msg[sizeof(lua_req.error)];
        snprintf(msg, sizeof(msg), "refused: up %u s, reboot needs %u", (unsigned)up_s,
                 (unsigned)LUA_REBOOT_MIN_S);
        req_error(msg);
        log_msg(SOS_LOG_KERN | SOS_LOG_WARNING, "%s asked for a reboot after %u s up: refused",
                job_tag, (unsigned)up_s);
        return;
    }
    const char *why = (const char *)lua_req.data;
    log_msg(SOS_LOG_KERN | SOS_LOG_NOTICE, "reboot (from Lua script %s%s%s)", job_tag,
            *why ? ": " : "", why);
    wdog_reboot();
}

// sos.hashfile: one request reads and hashes for at most this long (sums.c's STEP_US).
#define HASH_FILE_STEP_US 20000

// sos.hash, sos.hmac, sos.hashfile: one step (lua_hash.c).
static void serve_hash(void) {
    if (lua_req.offset == LUA_HASH_FILE) {      // result: the bytes hashed, 0 at the end
        char path[LUA_PATH_MAX];
        uint64_t got;
        if (!req_path(path)) {
            lua_hash_drop();
            return;
        }
        const char *err = lua_hash_file(path, lua_req.data, LUA_REQ_DATA, HASH_FILE_STEP_US, &got);
        if (err)
            req_error(err);
        else
            lua_req.result = (int64_t)got;
        return;
    }
    if (lua_req.len > LUA_REQ_DATA) {
        req_error("too long");
        return;
    }
    size_t n = 0;
    uint8_t digest[LUA_HASH_MAX];
    const char *err = lua_hash_step(lua_req.offset, lua_req.arg, lua_req.data, lua_req.len,
                                    digest, &n);
    if (err) {
        req_error(err);
        return;
    }
    memcpy(lua_req.data, digest, n);
    lua_req.len = (uint32_t)n;
}

// sos.wall(msg): to every shell, from the script (its tag as the sender). It shares the log's
// limit of SCRIPT_LOG_PER_SEC, and never waits: a full ring of unread messages is an error.
static void serve_wall(void) {
    lua_req.data[LUA_REQ_DATA - 1] = 0;
    if (!script_alive()) {
        req_error("the script's session has ended");
        return;
    }
    if (!log_quota_take(&script_quota, SCRIPT_LOG_PER_SEC)) {
        req_error("more than 10 messages a second");
        return;
    }
    int n = chat_wall_from(job_tag, (const char *)lua_req.data);
    if (n < 0)
        req_error("other shells haven't read the last messages yet: try again");
    else
        lua_req.result = n;
}

// sos.time.sync(): as `time sync` (which any shell may run), but at most one started by scripts
// every LUA_TIMESYNC_MIN_S, so a script in a loop can't hammer the NTS server.
static void serve_timesync(void) {
    static uint64_t last_us;
    static bool started;
    uint64_t now = time_us_64();
    if (started && now - last_us < (uint64_t)LUA_TIMESYNC_MIN_S * 1000000) {
        char msg[sizeof(lua_req.error)];
        snprintf(msg, sizeof(msg), "a script started one %u s ago (at most one per %u s)",
                 (unsigned)((now - last_us) / 1000000), (unsigned)LUA_TIMESYNC_MIN_S);
        req_error(msg);
        return;
    }
    if (!timesync_request_nts()) {
        req_error("a sync is already running");
        return;
    }
    started = true;
    last_us = now;
    log_msg(SOS_LOG_DAEMON | SOS_LOG_INFO, "%s started a time sync", job_tag);
}

// sos.random(n): bytes from wolfCrypt's DRBG, seeded by the chip's TRNG (core/random.h).
static void serve_random(void) {
    uint32_t n = lua_req.offset;
    if (n > LUA_REQ_DATA || !random_bytes(NULL, lua_req.data, n)) {
        req_error("no random bytes");
        return;
    }
    lua_req.len = n;
}

// sos.sensors, sos.mem, ...: a read-only table (lua_health.c).
static void serve_info(void) {
    size_t n = lua_req.offset < LH_TABLES
        ? lua_health_fill((lua_health_table_t)lua_req.offset, lua_req.arg, lua_req.data,
                          LUA_REQ_DATA) : 0;
    if (!n)
        req_error("no such table, or it didn't fit");
    lua_req.len = (uint32_t)n;
}

// --- HTTPS (sos.http) ----------------------------------------------------------------------------
//
// The script's request (one at a time) belongs to core 0; core 1 sees only what the answers copy
// out. HTTPS requests are answered from the main loop only (lua_host_poll(true)), never from
// inside a shell job: the TLS handshake needs stack, and an SSH session's job runs deep inside
// sshd_poll().

static http_req_t *http;                // the script's request, if any
static bool http_started;               // the pending LR_HTTP_REQUEST has been started

static void http_drop(void) {
    http_close(http);
    http = NULL;
}

_Static_assert(HTTP_HEADERS_MAX <= LUA_REQ_DATA, "the headers must fit in one answer");
_Static_assert(HTTP_REASON_MAX < LUA_PATH_MAX, "the reason must fit in lua_req.path");

// Answer LR_HTTP_REQUEST once the headers are in.
static bool serve_http_request(void) {
    if (!http_started) {
        const char *err;
        http_drop();                            // one left over from a request the script abandoned
        http = http_start(lua_req.opts, &err);
        if (!http)
            return !req_error(err);
        http_started = true;
    }
    http_state_t st = http_poll(http);
    if (st == HTTP_FAILED) {
        req_error(http_error(http));
        http_drop();
        return true;
    }
    if (st < HTTP_BODY)
        return false;
    const http_resp_t *p = http_response(http);
    snprintf(lua_req.path, sizeof(lua_req.path), "%s", p->reason);
    memcpy(lua_req.data, p->hdr, p->hdr_len);
    lua_req.len = (uint32_t)p->hdr_len;
    lua_req.result = p->status;
    return true;
}

_Static_assert(sizeof(lua_http_stats_t) + HTTP_URL_MAX + 1 <= LUA_REQ_DATA,
               "the timings and the URL must fit in one answer");

static uint32_t clamp_u32(uint64_t v) {
    return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

// The finished request's timings and last URL, into the answer that ends its body (the handshake
// is truncated to whole milliseconds on core 1, as the `http` command prints it).
static void http_stats(void) {
    lua_http_stats_t s = {
        .redirects = http_redirects(http),
        .headers_ms = http_headers_ms(http),
        .elapsed_ms = http_elapsed_ms(http),
    };
    const tls_stats_t *ts = http_tls_stats(http);
    if (ts) {                                   // always, for https:// (the only scheme)
        s.tls = true;
        s.dates_unchecked = ts->dates_unchecked;
        s.handshake_us = clamp_u32(ts->handshake_us);
        s.longest_step_us = ts->longest_step_us;
        s.heap_peak = clamp_u32(ts->heap_peak);
        s.cert = ts->cert_known;
        s.cert_not_before = ts->cert_not_before;
        s.cert_not_after = ts->cert_not_after;
        memcpy(s.cert_subject, ts->cert_subject, sizeof(s.cert_subject));
        memcpy(s.cert_issuer, ts->cert_issuer, sizeof(s.cert_issuer));
    }
    memcpy(lua_req.data, &s, sizeof(s));
    snprintf((char *)lua_req.data + sizeof(s), HTTP_URL_MAX + 1, "%s", http_url(http));
}

// Answer LR_HTTP_READ with as much of the body as there is now, up to a full answer.
static bool serve_http_read(void) {
    if (!http)
        return !req_error("no request");
    uint32_t got = 0;
    while (got < LUA_REQ_DATA) {
        int n = http_read(http, lua_req.data + got, LUA_REQ_DATA - got);
        if (n <= 0)
            break;
        got += (uint32_t)n;
    }
    if (!got) {
        http_state_t st = http_poll(http);
        if (st == HTTP_FAILED) {                // cut short, or timed out
            req_error(http_error(http));
            http_drop();
            return true;
        }
        if (st != HTTP_DONE)
            return false;                       // nothing yet
        http_stats();
        http_drop();                            // the end: len 0, and the timings
    }
    lua_req.len = got;
    lua_req.result = got;
    return true;
}

// Returns true once the request is answered.
static bool serve_http(void) {
    if (lua_job.cancel || !script_alive()) {
        http_drop();
        return !req_error("interrupted");
    }
    switch (lua_req.type) {
    case LR_HTTP_REQUEST: return serve_http_request();
    case LR_HTTP_READ:    return serve_http_read();
    default:              http_drop(); return true;     // LR_HTTP_CLOSE
    }
}

static bool is_http(lua_req_type_t t) {
    return t == LR_HTTP_REQUEST || t == LR_HTTP_READ || t == LR_HTTP_CLOSE;
}

static bool is_net(lua_req_type_t t) {
    return t == LR_SCAN || t == LR_RESOLVE || t == LR_PING;
}

// sos.wifi.scan, sos.net.resolve, sos.ping (lua_net.c): true once answered.
static bool serve_net(void) {
    if (lua_job.cancel || !script_alive()) {
        lua_net_drop();
        return !req_error("interrupted");
    }
    lua_req.path[LUA_PATH_MAX - 1] = '\0';
    size_t len;
    const char *err;
    if (!lua_net_serve(lua_req.type, lua_req.path, lua_req.offset, lua_req.data, LUA_REQ_DATA,
                       &len, &err))
        return false;
    if (err)
        req_error(err);
    else
        lua_req.len = (uint32_t)len;
    return true;
}

// Answer the pending request, if it can be answered here and now.
static void serve(bool main_loop) {
    if (is_http(lua_req.type)) {
        if (!main_loop)
            return;
        lua_req.result = 0;
        lua_req.error[0] = '\0';
        if (!serve_http())
            return;                             // not yet: again next pass
        http_started = false;
    } else if (is_net(lua_req.type)) {
        if (!main_loop)
            return;
        lua_req.result = 0;
        lua_req.error[0] = '\0';
        if (!serve_net())
            return;
    } else {
        lua_req.result = 0;
        lua_req.error[0] = '\0';
        switch (lua_req.type) {
        case LR_LOG: {
            int level = (int)lua_req.offset;    // checked by sos.log on core 1; checked again
            if (level < SOS_LOG_ERR || level > SOS_LOG_DEBUG)
                level = SOS_LOG_NOTICE;
            script_log(level, (const char *)lua_req.data);
            break;
        }
        case LR_TIME: {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            if (tv.tv_sec < 1704067200)         // before 2024: not set
                req_error("the clock isn't set");
            else
                lua_req.result = tv.tv_sec;
            break;
        }
        case LR_READ:   serve_read(); break;
        case LR_WRITE:  serve_write(); break;
        case LR_LIST:   serve_list(); break;
        case LR_REMOVE: serve_change(do_unlink); break;
        case LR_MKDIR:  serve_change(do_mkdir); break;
        case LR_RENAME: serve_rename(); break;
        case LR_STAT:   serve_stat(); break;
        case LR_INFO:   serve_info(); break;
        case LR_WALL:   serve_wall(); break;
        case LR_TIMESYNC: serve_timesync(); break;
        case LR_RANDOM: serve_random(); break;
        case LR_REBOOT: serve_reboot(); break;
        case LR_HASH:   serve_hash(); break;
        default:        req_error("unknown request"); break;
        }
    }
    __dmb();
    lua_req.done = true;
    __sev();
}

// --- the running script ----------------------------------------------------------------------

static bool repl;                       // the owner is at the Lua prompt (`lua` alone)
static void repl_answer(shell_t *sh, char *answer, void *arg);

// The request payload and the output ring: one block, there from start_job() until finish().
// Core 1 uses them only between LJ_START and LJ_DONE.
static bool alloc_buffers(void) {
    uint8_t *block = heap_alloc(HEAP_LUA, LUA_REQ_DATA + LUA_OUT_SIZE);
    if (!block)
        return false;
    lua_req.data = block;
    lua_out.buf = (char *)block + LUA_REQ_DATA;
    return true;
}

static void free_buffers(void) {
    lua_out.head = lua_out.tail = 0;    // nothing left to drain from a buffer that is going
    heap_release(HEAP_LUA, lua_req.data);
    lua_req.data = NULL;
    lua_out.buf = NULL;
}

static void finish(void) {
    script_log_dropped();
    http_drop();
    http_started = false;
    lua_net_drop();                     // a scan, lookup or ping the script didn't wait out
    lua_hash_drop();                    // one the script didn't finish
    heap_release(HEAP_LUA, owner_code);
    owner_code = NULL;
    lua_job.code = NULL;
    free_buffers();                     // core 1 is done with them: the job is LJ_DONE
    owner = NULL;
    repl = false;
    background = false;
    __dmb();
    lua_job.state = LJ_IDLE;
}

// --- background scripts --------------------------------------------------------------------------
//
// `lua -b <file>`, or /autorun.lua at boot: no shell, so the output goes to the log, a line at a
// time (INFO, so it's in `dmesg` but not on the console), and the end is logged too.

// Lines reach the log through script_log(), so they share sos.log's rate limit.

static char bg_line[LOG_LINE_MAX + 1];
static size_t bg_len;

// Room for a line's text in one log message, after the "<script>: " prefix.
static size_t bg_room(void) {
    size_t prefix = strlen(job_tag) + 2;
    return prefix + 40 < LOG_LINE_MAX ? LOG_LINE_MAX - prefix : 40;
}

static void bg_flush_line(void) {
    if (!bg_len)
        return;
    bg_line[bg_len] = '\0';
    script_log(SOS_LOG_INFO, bg_line);
    bg_len = 0;
}

// Move what core 1 has written so far into the log. Only up to where head was on entry: core 1
// refills the buffer as it's drained, so "until it's empty" could go on forever.
static void bg_drain(void) {
    __dmb();
    uint32_t end = lua_out.head;
    size_t room = bg_room();
    while (lua_out.tail != end) {
        char c = lua_out.buf[lua_out.tail % LUA_OUT_SIZE];
        __dmb();
        lua_out.tail++;
        if (c == '\n' || bg_len == room) {     // a long line continues on the next one
            bg_flush_line();
            if (c == '\n')
                continue;
        }
        bg_line[bg_len++] = ((unsigned char)c < 0x20 && c != '\t') || c == 0x7f ? '.' : c;
    }
    __sev();                                    // core 1 may be waiting for room
}

// The error on its own line (WARNING); its traceback, if any, a line at a time (INFO).
static void bg_log_error(unsigned secs) {
    const char *e = lua_job.error, *nl = strchr(e, '\n');
    log_msg(SOS_LOG_USER | SOS_LOG_WARNING, "lua: %s failed after %u s: %.*s", job_desc, secs,
            nl ? (int)(nl - e) : (int)strlen(e), e);
    while (nl) {
        e = nl + 1;
        nl = strchr(e, '\n');
        int len = nl ? (int)(nl - e) : (int)strlen(e);
        while (len && (*e == '\t' || *e == ' '))
            e++, len--;
        if (len)
            log_msg(SOS_LOG_USER | SOS_LOG_INFO, "%s:   %.*s", job_tag, len, e);
    }
}

static void bg_poll(void) {
    bg_drain();
    if (lua_job.state != LJ_DONE || lua_out.head != lua_out.tail)
        return;
    bg_flush_line();
    script_log_dropped();
    unsigned s = (unsigned)((time_us_64() - job_start_us) / 1000000);
    if (lua_job.ok)
        log_msg(SOS_LOG_USER | SOS_LOG_NOTICE, "lua: %s finished after %u s", job_desc, s);
    else if (lua_job.cancel)
        log_msg(SOS_LOG_USER | SOS_LOG_NOTICE, "lua: %s stopped after %u s", job_desc, s);
    else
        bg_log_error(s);
    finish();
}

static void autorun_check(void);

void lua_host_poll(bool main_loop) {
    autorun_check();
    if (lua_req.pending && !lua_req.done)
        serve(main_loop);
    // At the prompt, the owner's shell is always either answering our question or running one of
    // our jobs. If it's doing neither, its session ended (shell_end() or shell_init() dropped the
    // question): close the prompt.
    if (repl && owner && lua_job.state != LJ_IDLE && !owner->job && owner->answer != repl_answer) {
        owner = NULL;
        lua_job.repl_quit = true;
        lua_job.cancel = true;
        __sev();
    }
    // The prompt ended while its shell waited for a line: stopped from elsewhere (`lua stop`).
    // Nothing on that shell would notice, so close it here and give the shell back.
    if (repl && owner && lua_job.state == LJ_DONE && !owner->job) {
        shell_t *sh = owner;
        lua_out.tail = lua_out.head;
        shell_hide_input(sh);
        shell_cancel_question(sh);
        shell_printf(sh, "lua: the prompt was stopped ('lua stop')\n");
        shell_show_input(sh);
        finish();
        return;
    }
    if (background && lua_job.state != LJ_IDLE) {
        bg_poll();
    } else if (!owner && lua_job.state != LJ_IDLE) {
        lua_out.tail = lua_out.head;            // nobody to show it to
        __sev();
        if (lua_job.state == LJ_DONE)
            finish();
    }
}

// Copy the script's output to the shell, as much as its channel takes now.
static void drain(shell_t *sh) {
    for (;;) {
        __dmb();
        uint32_t avail = lua_out.head - lua_out.tail;
        int space = shell_write_space(sh) / 2;  // room for "\n" -> "\r\n"
        if (!avail || space <= 0)
            break;
        uint32_t start = lua_out.tail % LUA_OUT_SIZE;
        uint32_t n = avail;
        if (n > LUA_OUT_SIZE - start)
            n = LUA_OUT_SIZE - start;           // up to the end of the ring
        if (n > (uint32_t)space)
            n = (uint32_t)space;
        shell_write(sh, lua_out.buf + start, n);
        __dmb();
        lua_out.tail += n;
        __sev();                                // core 1 may be waiting for room
    }
}

// Shell job: show the script's output; when it's done, say how it ended.
static bool lua_step(shell_t *sh, void *arg, bool cancel) {
    (void)arg;
    if (cancel) {                               // Ctrl-C, or the session ended
        lua_job.cancel = true;
        __sev();
        owner = NULL;                           // lua_host_poll() finishes up
        return false;
    }
    lua_host_poll(false);                       // answer a request without waiting a pass
    drain(sh);
    if (lua_job.state != LJ_DONE || lua_out.head != lua_out.tail)
        return true;
    if (!lua_job.ok)
        shell_printf(sh, "lua: %s\n", lua_job.error);
    finish();
    return false;
}


// --- the interactive prompt (`lua` alone) ------------------------------------------------------
//
// The prompt is a question (shell_ask_eof) whose answers are lines of Lua. Each line runs as a
// short shell job (repl_step) that hands it to core 1 and shows its output; then the prompt is
// asked again, "> " or ">> " if the statement continues. Ctrl-C while a line runs interrupts it
// (and drops an unfinished statement); Ctrl-C at the prompt drops the unfinished statement.
// Ctrl-D on an empty line, or "exit", leaves.

_Static_assert(LUA_LINE_MAX >= SHELL_LINE_MAX, "a shell line must fit in lua_job.repl_line");

static char repl_pending[LUA_LINE_MAX + 1];    // the line to hand over
static bool repl_sent;                          // ...handed over to core 1
static bool repl_reset_next;                    // drop the unfinished statement with the next line
static bool repl_more;                          // the statement continues: ">> "

// Tab at the prompt: core 1, waiting for the next line, lists the names that could finish the
// one being typed (lua/lua_complete.h: raw table access only, so none of the script's code
// runs). It takes microseconds; if core 1 hasn't answered within REPL_TAB_WAIT_US (or is still
// busy with an earlier Tab, or stopping a line), nothing is offered this time, and the next
// line waits for it as it waits for a line interrupted with Ctrl-C (repl_step).
#define REPL_TAB_WAIT_US 20000

static bool repl_tab(shell_t *sh, shell_value_fn fn, void *arg) {
    if (owner != sh || !repl || lua_job.state != LJ_RUNNING || lua_job.repl_cmd || !lua_req.data)
        return false;
    size_t s = sh->line_len;
    while (s > 0 && strchr(LUA_COMPLETE_CHARS, sh->line[s - 1]))
        s--;
    size_t n = sh->line_len - s;            // <= SHELL_LINE_MAX <= LUA_LINE_MAX
    memcpy(lua_job.repl_line, sh->line + s, n);
    lua_job.repl_line[n] = '\0';
    lua_job.repl_complete = true;
    __dmb();
    lua_job.repl_cmd = true;
    __sev();
    uint64_t until = time_us_64() + REPL_TAB_WAIT_US;
    while (lua_job.repl_cmd && time_us_64() < until)
        tight_loop_contents();
    if (lua_job.repl_cmd)
        return false;
    __dmb();
    lua_job.repl_complete = false;
    const char *p = (const char *)lua_req.data, *end = p + lua_job.repl_names_len;
    for (uint32_t i = 0; i < lua_job.repl_names && p < end; i++) {
        fn(p, NULL, arg);
        p += strlen(p) + 1;
    }
    return false;
}

static void repl_ask(shell_t *sh) {
    shell_ask_eof(sh, repl_more ? ">> " : "> ", repl_answer, NULL);
    shell_ask_complete_chars(sh, repl_tab, LUA_COMPLETE_CHARS);
}

// Core 1 has ended the session (it couldn't start, or after exit): report and finish.
static bool repl_ended(shell_t *sh) {
    drain(sh);
    if (lua_out.head != lua_out.tail)
        return true;
    if (!lua_job.ok)
        shell_printf(sh, "lua: %s\n", lua_job.error);
    finish();
    return false;
}

// Shell job: run one line on core 1 and show its output.
static bool repl_step(shell_t *sh, void *arg, bool cancel) {
    (void)arg;
    if (cancel) {           // Ctrl-C (or the session ended: then lua_host_poll() closes the prompt)
        lua_job.cancel = true;
        __sev();
        repl_reset_next = true;
        repl_more = false;
        repl_ask(sh);
        return false;
    }
    lua_host_poll(false);
    if (lua_job.state == LJ_DONE)
        return repl_ended(sh);
    if (!repl_sent) {
        if (lua_job.repl_cmd)
            return true;                        // still stopping a line interrupted with Ctrl-C
        lua_out.tail = lua_out.head;            // what that line printed after Ctrl-C
        lua_job.cancel = false;
        memcpy(lua_job.repl_line, repl_pending, sizeof(lua_job.repl_line));
        lua_job.repl_complete = false;          // a line, even after a Tab that timed out
        lua_job.repl_reset = repl_reset_next;
        repl_reset_next = false;
        __dmb();
        lua_job.repl_cmd = true;
        __sev();
        repl_sent = true;
        return true;
    }
    drain(sh);
    __dmb();
    if (lua_job.repl_cmd || lua_out.head != lua_out.tail)
        return true;
    http_drop();                                // one that an error in the line left open
    lua_hash_drop();                            // and a hash (sos.hashfile's file) it left
    repl_more = lua_job.repl_incomplete;
    repl_ask(sh);
    return false;
}

// Shell job: wait for core 1 to close the session.
static bool repl_quit_step(shell_t *sh, void *arg, bool cancel) {
    (void)arg;
    if (cancel) {
        owner = NULL;                           // lua_host_poll() finishes up
        return false;
    }
    lua_host_poll(false);
    if (lua_job.state != LJ_DONE) {
        drain(sh);
        return true;
    }
    return repl_ended(sh);
}

static bool is_exit(const char *s) {
    while (*s == ' ')
        s++;
    if (strncmp(s, "exit", 4))
        return false;
    for (s += 4; *s == ' '; s++)
        ;
    return !*s;
}

static void repl_answer(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (answer == SHELL_EOF || (answer && !repl_more && is_exit(answer))) {
        lua_job.repl_quit = true;
        lua_job.cancel = true;                  // in case a line is still stopping
        __sev();
        shell_run_job(sh, repl_quit_step, NULL);
        return;
    }
    if (!answer) {                              // Ctrl-C at the prompt
        repl_reset_next = true;
        repl_more = false;
        repl_ask(sh);
        return;
    }
    snprintf(repl_pending, sizeof(repl_pending), "%s", answer);
    repl_sent = false;
    shell_run_job(sh, repl_step, NULL);
}

// --- the lua command ---------------------------------------------------------------------------

// Start a script on core 1: code (`lua -e`), a file, or the prompt (both NULL). Returns NULL, or
// why it couldn't start.
static const char *start_job(shell_t *sh, const char *code, const char *path, const char *cwd,
                             bool bg) {
    if (lua_job.state != LJ_IDLE)
        return "a script is already running (one at a time; see 'lua ps')";
    if (!lua_core1_start())
        return "out of memory starting core 1";
    if (!alloc_buffers())
        return "out of memory";
    memset(&lua_job, 0, sizeof(lua_job));
    if (code) {
        owner_code = heap_strdup(HEAP_LUA, code);
        if (!owner_code) {
            free_buffers();
            return "out of memory";
        }
        lua_job.code = owner_code;
        lua_job.code_len = strlen(owner_code);
        snprintf(job_desc, sizeof(job_desc), "lua -e %s", code);
        snprintf(job_tag, sizeof(job_tag), "lua -e");
    } else if (path) {
        snprintf(lua_job.path, sizeof(lua_job.path), "%s", path);
        snprintf(job_desc, sizeof(job_desc), "%s", path);
        snprintf(job_tag, sizeof(job_tag), "%s", path);
    } else {
        lua_job.repl = true;
        snprintf(job_desc, sizeof(job_desc), "the prompt");
        snprintf(job_tag, sizeof(job_tag), "lua prompt");
    }
    snprintf(lua_job.cwd, sizeof(lua_job.cwd), "%s", cwd);
    snprintf(lua_job.tz_name, sizeof(lua_job.tz_name), "%s", settings()->tz_name);
    snprintf(lua_job.tz_rule, sizeof(lua_job.tz_rule), "%s", settings()->tz_rule);
    lua_job.mem_limit = LUA_MEM_LIMIT;
    lua_out.head = lua_out.tail = 0;
    lua_req.pending = lua_req.done = false;
    owner = bg ? NULL : sh;
    background = bg;
    bg_len = 0;
    script_quota = (log_quota_t){0};
    job_start_us = time_us_64();
    __dmb();
    lua_job.state = LJ_START;
    __sev();
    return NULL;
}

// --- autorun -------------------------------------------------------------------------------------
//
// A few seconds after boot, /autorun.lua (if the card has one) starts as a background script. Not
// after a crash or watchdog reset, though, so a script that brings the device down can't do it
// again at every boot: then it's skipped (and logged), and the next normal boot runs it again.

#define AUTORUN_PATH "/autorun.lua"
#define AUTORUN_DELAY_US 3000000

static bool autorun_done;

static void autorun_check(void) {
    if (autorun_done || time_us_64() < AUTORUN_DELAY_US)
        return;
    autorun_done = true;
    FILINFO fi;
    if (!sdcard_mount(NULL) || f_stat(AUTORUN_PATH, &fi) != FR_OK)
        return;
    const char *why = fault_last() ? fault_last() : wdog_caused_reset() ? "watchdog timeout" : NULL;
    if (why) {
        log_msg(SOS_LOG_USER | SOS_LOG_WARNING,
                "lua: %s not started: the last reset was a %s", AUTORUN_PATH, why);
        return;
    }
    const char *err = start_job(NULL, NULL, AUTORUN_PATH, "/", true);
    if (err)
        log_msg(SOS_LOG_USER | SOS_LOG_WARNING, "lua: %s not started: %s", AUTORUN_PATH, err);
    else
        log_msg(SOS_LOG_USER | SOS_LOG_NOTICE, "lua: running %s in the background", AUTORUN_PATH);
}

// --- the lua command ---------------------------------------------------------------------------

bool lua_host_running(void) {
    return lua_job.state != LJ_IDLE;
}

uint32_t lua_host_job_seconds(void) {
    return lua_job.state == LJ_IDLE ? 0 : (uint32_t)((time_us_64() - job_start_us) / 1000000);
}

static void lua_ps(shell_t *sh) {
    if (lua_job.state == LJ_IDLE) {
        shell_printf(sh, "no script running\n");
        return;
    }
    unsigned s = (unsigned)((time_us_64() - job_start_us) / 1000000);
    const char *where = background ? "in the background" : owner ? owner->io->name : "stopping";
    shell_printf(sh, "%s: %s, %s, for %u s\n", job_desc, where,
                 lua_job.cancel ? "stopping" : "running", s);
}

static void lua_stop(shell_t *sh) {
    if (lua_job.state == LJ_IDLE) {
        shell_printf(sh, "no script running\n");
        return;
    }
    shell_printf(sh, "stopping %s\n", job_desc);
    if (repl)
        lua_job.repl_quit = true;               // the prompt: leave it, not just the line
    lua_job.cancel = true;
    __sev();
}

void lua_command(shell_t *sh, int argc, char **argv) {
    const char *opt = argc > 1 ? argv[1] : NULL;
    if (opt && (!strcmp(opt, "ps") || !strcmp(opt, "stop"))) {
        if (argc != 2)
            shell_printf(sh, "usage: lua %s\n", opt);
        else if (!strcmp(opt, "ps"))
            lua_ps(sh);
        else
            lua_stop(sh);
        return;
    }
    const char *code = NULL, *file = NULL, *err;
    char path[LUA_PATH_MAX];
    bool bg = false;
    if (opt && !strcmp(opt, "-e")) {
        // The code is free text: the words after -e, single-spaced (`lua -e print(1 +  1)`), or
        // one quoted word kept as it is (`lua -e 'print("a  b")'`).
        code = words_join(argc - 1, argv + 1);
        if (!code) {
            shell_printf(sh, "usage: lua -e <code>\n");
            return;
        }
    } else if (opt) {
        bg = !strcmp(opt, "-b");
        file = argv[bg ? 2 : 1];
        if (argc != (bg ? 3 : 2) || !*file) {
            shell_printf(sh, bg ? "usage: lua -b <file>\n"
                                : "usage: lua [<file> | -b <file> | -e <code> | ps | stop]\n");
            return;
        }
        if (!path_resolve(sh->cwd, file, path, sizeof(path))) {
            shell_printf(sh, "lua: path too long\n");
            return;
        }
    }
    err = start_job(sh, code, file ? path : NULL, sh->cwd, bg);
    if (err) {
        shell_printf(sh, "lua: %s\n", err);
        return;
    }
    if (bg) {
        shell_printf(sh, "%s runs in the background; output goes to the log ('dmesg'). "
                     "'lua ps', 'lua stop'.\n", job_desc);
        return;
    }
    if (lua_job.repl) {
        repl = true;
        repl_more = repl_reset_next = false;
        shell_printf(sh, "%s on core 1. Ctrl-C interrupts; Ctrl-D or 'exit' leaves.\n", LUA_RELEASE);
        repl_ask(sh);
        return;
    }
    shell_run_job(sh, lua_step, NULL);
}
