// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Shared state between the Lua interpreter on core 1 (src/lua/lua_core1.c) and core 0
// (src/lua/lua_host.c). Core 1 never touches the SD card, the network, the log or the shells: it
// asks core 0 through a single request mailbox and writes script output into a ring buffer that
// core 0 drains into the shell that started the script.
//
// Hand-over is with flags plus memory barriers (__dmb) and events (__sev/__wfe): one side fills
// the data, then sets the flag; the other side sees the flag, then reads the data.

#ifndef SOS_LUA_BRIDGE_H
#define SOS_LUA_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LUA_PATH_MAX   256
#define LUA_REQ_DATA   4096         // payload per request (file data, directory entries)
#define LUA_OUT_SIZE   2048         // output ring buffer
#define LUA_LINE_MAX   256          // one line at the interactive prompt (SHELL_LINE_MAX)

// --- one script at a time -------------------------------------------------------------------

enum { LJ_IDLE, LJ_START, LJ_RUNNING, LJ_DONE };

typedef struct {
    volatile int state;             // core 0: IDLE -> START; core 1: START -> RUNNING -> DONE
    volatile bool cancel;           // core 0 asks the script to stop (checked by a Lua hook)
    // Set by core 0 before START:
    const char *code;               // `lua -e` source (core 0 owns it), or NULL: run `path`
    size_t code_len;
    char path[LUA_PATH_MAX];        // script file (absolute)
    char cwd[LUA_PATH_MAX];         // working directory for relative paths in the script
    uint32_t mem_limit;             // bytes the script may allocate
    bool repl;                      // interactive prompt (`lua` alone) instead of code or path
    // Interactive prompt, one line at a time: core 0 fills repl_line and repl_reset, then sets
    // repl_cmd; core 1 runs the line, sets repl_incomplete and clears repl_cmd.
    volatile bool repl_cmd;
    volatile bool repl_quit;        // core 0: leave the prompt (core 1 then ends the job)
    bool repl_reset;                // drop the unfinished statement before this line (Ctrl-C)
    bool repl_incomplete;           // the statement continues on the next line
    char repl_line[LUA_LINE_MAX + 1];
    // Set by core 1 before DONE:
    bool ok;
    char error[256];
    uint32_t mem_peak;
    uint32_t run_ms;
} lua_job_t;

extern lua_job_t lua_job;

// --- requests: core 1 asks, core 0 answers ----------------------------------------------------

typedef enum {
    LR_LOG,                         // data: message, offset: level (SOS_LOG_ERR ... DEBUG)
    LR_TIME,                        // -> result: seconds since 1970 (0 if the clock isn't set)
    LR_READ,                        // path, offset -> data/len (len 0: end of file)
    LR_WRITE,                       // path, data/len, append (offset > 0: append)
    LR_LIST,                        // path, offset (entries to skip) -> data: entries, count
    LR_REMOVE,                      // path
    LR_MKDIR,                       // path
    // HTTPS (net/http.h), one request at a time, answered from the main loop once there's
    // something to say, so these can take a while. Ctrl-C answers them at once with an error.
    LR_HTTP_REQUEST,                // opts: an http_opts_t -> once the headers are in:
                                    // result: status, path: reason, data/len: headers
    LR_HTTP_READ,                   // -> data/len: body bytes (len 0: the end)
    LR_HTTP_CLOSE,                  // drop the request (the body isn't wanted)
} lua_req_type_t;

typedef struct {
    volatile bool pending;          // core 1 -> core 0: request ready
    volatile bool done;             // core 0 -> core 1: answer ready
    lua_req_type_t type;
    char path[LUA_PATH_MAX];
    uint32_t offset;
    uint32_t len;
    const void *opts;               // LR_HTTP_REQUEST: core 1's http_opts_t, and the strings it
                                    // points at, which core 0 reads while core 1 waits
    int64_t result;                 // < 0: failed (error has why); LUA_REQ_NOT_FOUND: no such file
    char error[64];
    uint8_t data[LUA_REQ_DATA];
} lua_req_t;

extern lua_req_t lua_req;

// LR_READ result when the file or a directory on its path doesn't exist (require tries the next
// path; any other failure, such as no SD card, stops the search).
#define LUA_REQ_NOT_FOUND (-2)

// LR_LIST packs entries into data as: uint32 size, uint8 is_dir, name, NUL; result = count,
// len = bytes used. A listing that doesn't fit continues with offset = entries already returned.

// LR_HTTP_REQUEST packs the headers into data as http_resp_t stores them: "name:value\n" each,
// names in lower case.

// --- output: core 1 writes, core 0 reads -------------------------------------------------------

typedef struct {
    volatile uint32_t head;         // written by core 1 (bytes produced)
    volatile uint32_t tail;         // written by core 0 (bytes consumed)
    char buf[LUA_OUT_SIZE];
} lua_out_t;

extern lua_out_t lua_out;

#endif
