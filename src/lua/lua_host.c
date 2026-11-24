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

#include "core/log.h"
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

static void finish(void) {
    free(owner_code);
    owner_code = NULL;
    lua_job.code = NULL;
    owner = NULL;
    __dmb();
    lua_job.state = LJ_IDLE;
}

void lua_host_poll(void) {
    if (lua_req.pending && !lua_req.done)
        serve();
    if (!owner && lua_job.state != LJ_IDLE) {
        lua_out.tail = lua_out.head;            // nobody to show it to
        __sev();
        if (lua_job.state == LJ_DONE)
            finish();
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
    if (lua_job.state != LJ_DONE || lua_out.head != lua_out.tail)
        return true;
    if (!lua_job.ok)
        shell_printf(sh, "lua: %s\n", lua_job.error);
    finish();
    return false;
}

void lua_command(shell_t *sh, char *args) {
    if (!args) {
        shell_printf(sh, "usage: lua <file> | lua -e <code>\n");
        return;
    }
    if (lua_job.state != LJ_IDLE) {
        shell_printf(sh, "lua: a script is already running (one at a time)\n");
        return;
    }
    if (!lua_core1_start()) {
        shell_printf(sh, "lua: out of memory starting core 1\n");
        return;
    }
    memset(&lua_job, 0, sizeof(lua_job));
    if (!strncmp(args, "-e ", 3)) {
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
    shell_run_job(sh, lua_step, NULL);
}
