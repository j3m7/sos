// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Core 0's side of Lua: the `lua` shell command, and the requests the script on core 1 makes
// (files on the SD card, log, time). Requests are answered from the main loop, one per pass,
// each a bounded amount of work, so the watchdog, SSH and the console keep going whatever the
// script does. The script's output goes to the shell that started it, as fast as that shell's
// channel takes it.

#include "lua/lua_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "hardware/sync.h"
#include "pico/stdlib.h"

#include "ff.h"
#include "lua.h"

#include "core/fault.h"
#include "core/log.h"
#include "core/wdog.h"
#include "files/files.h"
#include "files/path.h"
#include "files/sdcard.h"
#include "lua/lua_bridge.h"
#include "lua/lua_core1.h"

#define LUA_MEM_LIMIT (128 * 1024)      // heap a script may use

lua_job_t lua_job;
lua_req_t lua_req;
lua_out_t lua_out;

static shell_t *owner;                  // shell that started the script; NULL once it's gone
static char *owner_code;                // `lua -e` source, freed when the script ends
static bool background;                 // started with `lua -b` or at boot: output to the log
static char job_desc[48];               // what's running, for `lua ps` and the log
static uint64_t job_start_us;

// The script may still act: its shell is there, or it runs in the background. A script whose
// shell has gone has been stopped and changes nothing more.
static bool script_alive(void) {
    return owner || background;
}

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
static FRESULT do_unlink(const TCHAR *p) { return f_unlink(p); }

static void serve(void) {
    lua_req.result = 0;
    lua_req.error[0] = '\0';
    switch (lua_req.type) {
    case LR_LOG:
        log_msg(SOS_LOG_NOTICE, "lua: %s", (const char *)lua_req.data);
        break;
    case LR_TIME: {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        if (tv.tv_sec < 1704067200)             // before 2024: not set
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
    __dmb();
    lua_req.done = true;
    __sev();
}

// --- the running script ----------------------------------------------------------------------

static bool repl;                       // the owner is at the Lua prompt (`lua` alone)
static void repl_answer(shell_t *sh, char *answer, void *arg);

static void finish(void) {
    free(owner_code);
    owner_code = NULL;
    lua_job.code = NULL;
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

// At most BG_LINES_PER_SEC lines a second reach the log; beyond that they're counted and dropped,
// so a script printing in a loop doesn't push everything else (failed logins, warnings) out of
// the log history.
#define BG_LINES_PER_SEC 10

static char bg_line[LOG_LINE_MAX + 1];
static size_t bg_len;
static uint64_t bg_window_us;           // start of the current second
static unsigned bg_window_lines, bg_dropped;

// Room for a line's text in one log message, after the "<script>: " prefix.
static size_t bg_room(void) {
    size_t prefix = strlen(job_desc) + 2;
    return prefix + 40 < LOG_LINE_MAX ? LOG_LINE_MAX - prefix : 40;
}

static void bg_report_dropped(void) {
    if (bg_dropped)
        log_msg(SOS_LOG_INFO, "%s: ... %u lines dropped (more than %d a second)", job_desc,
                bg_dropped, BG_LINES_PER_SEC);
    bg_dropped = 0;
}

static void bg_flush_line(void) {
    if (!bg_len)
        return;
    uint64_t now = time_us_64();
    if (now - bg_window_us >= 1000000) {
        bg_report_dropped();
        bg_window_us = now;
        bg_window_lines = 0;
    }
    if (bg_window_lines < BG_LINES_PER_SEC) {
        bg_line[bg_len] = '\0';
        log_msg(SOS_LOG_INFO, "%s: %s", job_desc, bg_line);
        bg_window_lines++;
    } else {
        bg_dropped++;
    }
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
    log_msg(SOS_LOG_WARNING, "lua: %s failed after %u s: %.*s", job_desc, secs,
            nl ? (int)(nl - e) : (int)strlen(e), e);
    while (nl) {
        e = nl + 1;
        nl = strchr(e, '\n');
        int len = nl ? (int)(nl - e) : (int)strlen(e);
        while (len && (*e == '\t' || *e == ' '))
            e++, len--;
        if (len)
            log_msg(SOS_LOG_INFO, "%s:   %.*s", job_desc, len, e);
    }
}

static void bg_poll(void) {
    bg_drain();
    if (lua_job.state != LJ_DONE || lua_out.head != lua_out.tail)
        return;
    bg_flush_line();
    bg_report_dropped();
    unsigned s = (unsigned)((time_us_64() - job_start_us) / 1000000);
    if (lua_job.ok)
        log_msg(SOS_LOG_NOTICE, "lua: %s finished after %u s", job_desc, s);
    else if (lua_job.cancel)
        log_msg(SOS_LOG_NOTICE, "lua: %s stopped after %u s", job_desc, s);
    else
        bg_log_error(s);
    finish();
}

static void autorun_check(void);

void lua_host_poll(void) {
    autorun_check();
    if (lua_req.pending && !lua_req.done)
        serve();
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
    lua_host_poll();                            // answer a request without waiting a pass
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
    lua_host_poll();
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
    lua_host_poll();
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
    memset(&lua_job, 0, sizeof(lua_job));
    if (code) {
        owner_code = strdup(code);
        if (!owner_code)
            return "out of memory";
        lua_job.code = owner_code;
        lua_job.code_len = strlen(owner_code);
        snprintf(job_desc, sizeof(job_desc), "lua -e %s", code);
    } else if (path) {
        snprintf(lua_job.path, sizeof(lua_job.path), "%s", path);
        snprintf(job_desc, sizeof(job_desc), "%s", path);
    } else {
        lua_job.repl = true;
        snprintf(job_desc, sizeof(job_desc), "the prompt");
    }
    snprintf(lua_job.cwd, sizeof(lua_job.cwd), "%s", cwd);
    lua_job.mem_limit = LUA_MEM_LIMIT;
    lua_out.head = lua_out.tail = 0;
    lua_req.pending = lua_req.done = false;
    owner = bg ? NULL : sh;
    background = bg;
    bg_len = 0;
    bg_window_us = 0;
    bg_window_lines = bg_dropped = 0;
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
        log_msg(SOS_LOG_WARNING, "lua: %s not started: the last reset was a %s", AUTORUN_PATH, why);
        return;
    }
    const char *err = start_job(NULL, NULL, AUTORUN_PATH, "/", true);
    if (err)
        log_msg(SOS_LOG_WARNING, "lua: %s not started: %s", AUTORUN_PATH, err);
    else
        log_msg(SOS_LOG_NOTICE, "lua: running %s in the background", AUTORUN_PATH);
}

// --- the lua command ---------------------------------------------------------------------------

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

void lua_command(shell_t *sh, char *args) {
    if (args && !strcmp(args, "ps")) {
        lua_ps(sh);
        return;
    }
    if (args && !strcmp(args, "stop")) {
        lua_stop(sh);
        return;
    }
    const char *code = NULL, *err;
    char path[LUA_PATH_MAX];
    bool bg = false;
    if (args && !strncmp(args, "-e ", 3)) {
        code = args + 3;
    } else if (args && !strcmp(args, "-b")) {
        shell_printf(sh, "usage: lua -b <file>\n");
        return;
    } else if (args) {
        if (!strncmp(args, "-b ", 3)) {
            bg = true;
            args += 3;
            while (*args == ' ')
                args++;
        }
        if (!*args || !path_resolve(sh->cwd, args, path, sizeof(path))) {
            shell_printf(sh, *args ? "lua: path too long\n" : "usage: lua -b <file>\n");
            return;
        }
    }
    err = start_job(sh, code, (args && !code) ? path : NULL, sh->cwd, bg);
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
