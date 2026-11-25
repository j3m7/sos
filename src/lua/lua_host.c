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

#include "core/log.h"
#include "files/files.h"
#include "files/path.h"
#include "files/sdcard.h"
#include "lua/lua_bridge.h"
#include "lua/lua_core1.h"
#include "net/http.h"

#define LUA_MEM_LIMIT (128 * 1024)      // heap a script may use

lua_job_t lua_job;
lua_req_t lua_req;
lua_out_t lua_out;

static shell_t *owner;                  // shell that started the script; NULL once it's gone
static char *owner_code;                // `lua -e` source, freed when the script ends

// --- answering requests ----------------------------------------------------------------------

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
    // A script whose shell is gone has been stopped: it changes nothing more. Scripts get SFTP's
    // rights even when started on the console: the file may have come over SFTP, and the console's
    // extra rights (/certs) are for what the person there types, not for code they run.
    if (!owner || !files_write_allowed(NULL, path))
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
static FRESULT do_unlink(const TCHAR *p) { return f_unlink(p); }

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
        http_drop();                            // the end: len 0
    }
    lua_req.len = got;
    lua_req.result = got;
    return true;
}

// Returns true once the request is answered.
static bool serve_http(void) {
    if (lua_job.cancel || !owner) {
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
    } else {
        lua_req.result = 0;
        lua_req.error[0] = '\0';
        switch (lua_req.type) {
        case LR_LOG:
            log_msg(SOS_LOG_NOTICE, "lua: %s", (const char *)lua_req.data);
            break;
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

static void finish(void) {
    http_drop();
    http_started = false;
    free(owner_code);
    owner_code = NULL;
    lua_job.code = NULL;
    owner = NULL;
    repl = false;
    __dmb();
    lua_job.state = LJ_IDLE;
}

void lua_host_poll(bool main_loop) {
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
    if (!owner && lua_job.state != LJ_IDLE) {
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

static void repl_ask(shell_t *sh) {
    shell_ask_eof(sh, repl_more ? ">> " : "> ", repl_answer, NULL);
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

void lua_command(shell_t *sh, char *args) {
    if (lua_job.state != LJ_IDLE) {
        shell_printf(sh, "lua: a script is already running (one at a time)\n");
        return;
    }
    if (!lua_core1_start()) {
        shell_printf(sh, "lua: out of memory starting core 1\n");
        return;
    }
    memset(&lua_job, 0, sizeof(lua_job));
    if (!args) {
        lua_job.repl = true;
    } else if (!strncmp(args, "-e ", 3)) {
        owner_code = strdup(args + 3);
        if (!owner_code) {
            shell_printf(sh, "lua: out of memory\n");
            return;
        }
        lua_job.code = owner_code;
        lua_job.code_len = strlen(owner_code);
    } else if (!path_resolve(sh->cwd, args, lua_job.path, sizeof(lua_job.path))) {
        shell_printf(sh, "lua: path too long\n");
        return;
    }
    snprintf(lua_job.cwd, sizeof(lua_job.cwd), "%s", sh->cwd);
    lua_job.mem_limit = LUA_MEM_LIMIT;
    lua_out.head = lua_out.tail = 0;
    lua_req.pending = lua_req.done = false;
    owner = sh;
    __dmb();
    lua_job.state = LJ_START;
    __sev();
    if (lua_job.repl) {
        repl = true;
        repl_more = repl_reset_next = false;
        shell_printf(sh, "%s on core 1. Ctrl-C interrupts; Ctrl-D or 'exit' leaves.\n", LUA_RELEASE);
        repl_ask(sh);
        return;
    }
    shell_run_job(sh, lua_step, NULL);
}
