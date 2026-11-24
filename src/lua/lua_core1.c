// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The Lua interpreter on core 1. Runs one script at a time (lua_job, set up by core 0) in a
// fresh Lua state with the safe standard libraries (base, string, table, math, utf8, coroutine;
// no io, os, package or debug) plus `sos`, SOS's own library. Everything that touches the
// system goes to core 0 as a request (lua_bridge.h); this file only computes and waits.

#include <stdlib.h>
#include <string.h>

#include "hardware/sync.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "lua/lua_bridge.h"
#include "lua/lua_core1.h"

#define HOOK_EVERY 1000             // instructions between cancel checks

static uint32_t mem_used, mem_peak, mem_limit;

// --- talking to core 0 -----------------------------------------------------------------------

// Send the filled-in lua_req and wait for core 0's answer. Core 0 always answers, so this
// doesn't check for cancellation (the hook does, between instructions).
static void request(void) {
    lua_req.done = false;
    __dmb();
    lua_req.pending = true;
    __sev();
    while (!lua_req.done)
        __wfe();
    __dmb();
    lua_req.pending = false;
}

void sos_lua_write(const char *s, size_t len) {
    while (len) {
        uint32_t used = lua_out.head - lua_out.tail;
        if (used == LUA_OUT_SIZE) {             // full: core 0 drains (or discards) it
            if (lua_job.cancel)
                return;
            __wfe();
            continue;
        }
        size_t n = LUA_OUT_SIZE - used;
        if (n > len)
            n = len;
        for (size_t i = 0; i < n; i++)
            lua_out.buf[(lua_out.head + i) % LUA_OUT_SIZE] = s[i];
        __dmb();
        lua_out.head += (uint32_t)n;
        __sev();
        s += n;
        len -= n;
    }
}

// --- Lua state plumbing ----------------------------------------------------------------------

// All allocations go through here: the heap is shared with core 0 (malloc is mutex-protected
// with pico_multicore), and each script gets a hard limit.
static void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    if (!ptr)
        osize = 0;                              // osize is the object type when ptr is NULL
    if (nsize == 0) {
        free(ptr);
        mem_used -= (uint32_t)osize;
        return NULL;
    }
    if (nsize > osize && mem_used - osize + nsize > mem_limit)
        return NULL;                            // Lua raises "not enough memory"
    void *p = realloc(ptr, nsize);
    if (p) {
        mem_used = mem_used - (uint32_t)osize + (uint32_t)nsize;
        if (mem_used > mem_peak)
            mem_peak = mem_used;
    }
    return p;
}

static lua_State *main_L;               // the running script's state (its main thread)

static void hook(lua_State *L, lua_Debug *ar);

// Stop the script: Ctrl-C, or its shell has gone (lua_job.cancel). The error alone isn't enough,
// since a script's pcall can catch it and carry on. So from now on the hook runs before every
// instruction, in this thread and the main one, and raises the error again: the script ends as
// soon as it runs any code outside a pcall, and its main chunk is never inside one. (Coroutines
// copy the hook from the thread that creates them, and end the same way.)
static int interrupt(lua_State *L) {
    lua_sethook(L, hook, LUA_MASKCOUNT, 1);
    if (main_L && main_L != L)
        lua_sethook(main_L, hook, LUA_MASKCOUNT, 1);
    return luaL_error(L, "interrupted");
}

static void hook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    if (lua_job.cancel)
        interrupt(L);
}

static int traceback(lua_State *L) {
    const char *msg = lua_tostring(L, 1);
    luaL_traceback(L, L, msg ? msg : "(error object is not a string)", 1);
    return 1;
}

// --- the sos library -------------------------------------------------------------------------

// Put `p` into the request, resolved against the job's working directory (core 0 normalizes).
static void set_req_path(lua_State *L, const char *p) {
    // Absolute: as given. Relative: cwd + "/" + path (cwd "/" needs no extra separator).
    const char *dir = p[0] == '/' ? "" : lua_job.cwd;
    const char *sep = (p[0] == '/' || !strcmp(dir, "/")) ? "" : "/";
    size_t need = strlen(dir) + strlen(sep) + strlen(p) + 1;
    if (need > sizeof(lua_req.path))
        luaL_error(L, "path too long: %s", p);
    memcpy(lua_req.path, dir, strlen(dir));
    memcpy(lua_req.path + strlen(dir), sep, strlen(sep));
    memcpy(lua_req.path + strlen(dir) + strlen(sep), p, strlen(p) + 1);
}

// The same for a script's path argument.
static void set_path(lua_State *L, int arg) {
    set_req_path(L, luaL_checkstring(L, arg));
}

static int fail(lua_State *L) {
    lua_pushnil(L);
    lua_pushstring(L, lua_req.error);
    return 2;
}

static int l_print(lua_State *L) {
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        size_t len;
        const char *s = luaL_tolstring(L, i, &len);
        if (i > 1)
            sos_lua_write("\t", 1);
        sos_lua_write(s, len);
        lua_pop(L, 1);
    }
    sos_lua_write("\n", 1);
    return 0;
}

static int l_log(lua_State *L) {
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    if (len >= LUA_REQ_DATA)
        len = LUA_REQ_DATA - 1;
    lua_req.type = LR_LOG;
    memcpy(lua_req.data, s, len);
    lua_req.data[len] = '\0';
    request();
    return 0;
}

static int l_time(lua_State *L) {
    lua_req.type = LR_TIME;
    request();
    if (lua_req.result <= 0)
        return fail(L);
    lua_pushinteger(L, (lua_Integer)lua_req.result);
    return 1;
}

static int l_uptime(lua_State *L) {
    lua_pushnumber(L, (lua_Number)time_us_64() / 1e6);
    return 1;
}

static int l_sleep(lua_State *L) {
    lua_Integer ms = luaL_checkinteger(L, 1);
    absolute_time_t until = make_timeout_time_ms((uint32_t)(ms > 0 ? ms : 0));
    while (!time_reached(until)) {
        if (lua_job.cancel)
            return interrupt(L);
        sleep_ms(5);
    }
    return 0;
}

// Read the whole file at lua_req.path: pushes its contents and returns 0, or pushes nothing and
// returns the failed request's result (< 0; lua_req.error says why).
static int read_file(lua_State *L) {
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (uint32_t off = 0;; off += lua_req.len) {
        lua_req.type = LR_READ;
        lua_req.offset = off;
        request();
        if (lua_req.result < 0) {
            luaL_pushresult(&b);
            lua_pop(L, 1);
            return (int)lua_req.result;
        }
        if (lua_req.len == 0)
            break;
        luaL_addlstring(&b, (const char *)lua_req.data, lua_req.len);
    }
    luaL_pushresult(&b);
    return 0;
}

static int l_read(lua_State *L) {
    set_path(L, 1);
    if (read_file(L) < 0)
        return fail(L);
    return 1;
}

static int write_file(lua_State *L, bool append) {
    set_path(L, 1);
    size_t len;
    const char *s = luaL_checklstring(L, 2, &len);
    size_t done = 0;
    do {                                        // an empty string still creates/truncates
        size_t n = len - done < LUA_REQ_DATA ? len - done : LUA_REQ_DATA;
        lua_req.type = LR_WRITE;
        lua_req.offset = (append || done) ? 1 : 0;  // first chunk truncates unless appending
        lua_req.len = (uint32_t)n;
        memcpy(lua_req.data, s + done, n);
        request();
        if (lua_req.result < 0)
            return fail(L);
        done += n;
    } while (done < len);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_write(lua_State *L) { return write_file(L, false); }
static int l_append(lua_State *L) { return write_file(L, true); }

static int l_list(lua_State *L) {
    if (lua_gettop(L) == 0)
        lua_pushstring(L, ".");
    set_path(L, 1);
    lua_newtable(L);
    int n = 0;
    for (;;) {
        lua_req.type = LR_LIST;
        lua_req.offset = (uint32_t)n;
        request();
        if (lua_req.result < 0)
            return fail(L);
        if (lua_req.result == 0)
            break;
        const uint8_t *p = lua_req.data;
        for (int64_t i = 0; i < lua_req.result; i++) {
            uint32_t size;
            memcpy(&size, p, 4);
            bool dir = p[4];
            const char *name = (const char *)p + 5;
            lua_createtable(L, 0, 3);
            lua_pushstring(L, name);
            lua_setfield(L, -2, "name");
            lua_pushinteger(L, size);
            lua_setfield(L, -2, "size");
            lua_pushboolean(L, dir);
            lua_setfield(L, -2, "dir");
            lua_rawseti(L, -2, ++n);
            p += 5 + strlen(name) + 1;
        }
    }
    return 1;
}

static int simple(lua_State *L, lua_req_type_t type) {
    set_path(L, 1);
    lua_req.type = type;
    request();
    if (lua_req.result < 0)
        return fail(L);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_remove(lua_State *L) { return simple(L, LR_REMOVE); }
static int l_mkdir(lua_State *L) { return simple(L, LR_MKDIR); }

static const luaL_Reg sos_lib[] = {
    {"log", l_log},       {"time", l_time},     {"uptime", l_uptime}, {"sleep", l_sleep},
    {"read", l_read},     {"write", l_write},   {"append", l_append}, {"list", l_list},
    {"remove", l_remove}, {"mkdir", l_mkdir},   {NULL, NULL},
};

// setmetatable() without __gc. Lua runs finalizers with hooks off, so a finalizer that loops
// couldn't be stopped, and lua_close() runs them all when the script ends. A <close> variable
// does the same job and can be stopped. Lua only takes note of __gc when the metatable is set,
// so adding it to the metatable afterwards does nothing.
static int l_setmetatable(lua_State *L) {
    // The same checks as the base library's, here so errors name setmetatable.
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_argexpected(L, lua_type(L, 2) == LUA_TNIL || lua_type(L, 2) == LUA_TTABLE, 2, "nil or table");
    if (lua_type(L, 2) == LUA_TTABLE) {
        lua_pushliteral(L, "__gc");
        int t = lua_rawget(L, 2);
        lua_pop(L, 1);
        if (t != LUA_TNIL)
            return luaL_error(L, "__gc metamethods aren't supported (use a <close> variable)");
    }
    lua_pushvalue(L, lua_upvalueindex(1));      // the base library's setmetatable
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, 1);
    return 1;
}

// xpcall() whose message handler is skipped while the script is being stopped. Lua runs the
// handler for an error raised by the hook with hooks still off, so a handler that looped
// couldn't be stopped. The error object is returned unchanged instead.
static int guarded_handler(lua_State *L) {
    if (lua_job.cancel)
        return 1;
    lua_pushvalue(L, lua_upvalueindex(1));      // the script's handler
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, 1);
    return 1;
}

static int xpcall_done(lua_State *L, int status, lua_KContext ctx) {
    (void)status;
    (void)ctx;
    return lua_gettop(L);
}

static int l_xpcall(lua_State *L) {
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, guarded_handler, 1);
    lua_replace(L, 2);
    lua_pushvalue(L, lua_upvalueindex(1));      // the base library's xpcall
    lua_insert(L, 1);
    // With a continuation, so a coroutine can still yield inside xpcall.
    lua_callk(L, lua_gettop(L) - 1, LUA_MULTRET, 0, xpcall_done);
    return xpcall_done(L, LUA_OK, 0);
}

// --- require: modules from the SD card --------------------------------------------------------
//
// The standard package library, with its file searchers replaced: package.searchers is
// {preload, sd}. The sd searcher tries each template of package.path in turn ('?' replaced by the
// module name, '.' in the name by '/'), reading files through core 0 like sos.read. Text only:
// no precompiled chunks, and no C modules (no package.loadlib, empty cpath).

// Find `name` along `path`. Found: pushes the file's contents and its path, returns true. Not
// found: pushes "no file 'a'\n\tno file 'b'...", returns false. Other read errors (no SD card,
// ...) are raised: trying further paths would hide them.
static bool search(lua_State *L, const char *name, const char *path, const char *sep,
                   const char *rep) {
    if (*sep)
        name = luaL_gsub(L, name, sep, rep);
    else
        lua_pushstring(L, name);
    int tried = 0;
    for (const char *t = path; *t; ) {
        const char *end = strchr(t, ';');
        size_t len = end ? (size_t)(end - t) : strlen(t);
        if (len) {
            lua_pushlstring(L, t, len);
            const char *file = luaL_gsub(L, lua_tostring(L, -1), "?", name);
            lua_remove(L, -2);                          // the template
            set_req_path(L, file);
            int r = read_file(L);
            if (r == 0) {                               // stack: ..., file, contents
                lua_insert(L, -2);
                return true;                            // (the "no file" strings stay below)
            }
            if (r != LUA_REQ_NOT_FOUND)
                luaL_error(L, "cannot read '%s': %s", file, lua_req.error);
            lua_pushfstring(L, "%sno file '%s'", tried ? "\n\t" : "", file);
            lua_remove(L, -2);                          // the file name
            tried++;
            luaL_checkstack(L, 2, "too many package.path entries");
        }
        t += len + (end ? 1 : 0);
    }
    lua_concat(L, tried);
    return false;
}

// package.searchers[2]: load a Lua module from the SD card.
static int searcher_sd(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    lua_getfield(L, lua_upvalueindex(1), "path");
    const char *path = lua_tostring(L, -1);
    if (!path)
        return luaL_error(L, "'package.path' must be a string");
    if (!search(L, name, path, ".", LUA_DIRSEP))
        return 1;                                       // the "no file" list
    size_t len;
    const char *src = lua_tolstring(L, -2, &len);
    const char *file = lua_tostring(L, -1);
    const char *chunk = lua_pushfstring(L, "@%s", file);
    if (luaL_loadbufferx(L, src, len, chunk, "t") != LUA_OK)
        return luaL_error(L, "error loading module '%s' from file '%s':\n\t%s", name, file,
                          lua_tostring(L, -1));
    lua_pushstring(L, file);                            // passed to the loader, returned by require
    return 2;
}

// package.searchpath(name, path [, sep [, rep]]) for the SD card.
static int l_searchpath(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    const char *path = luaL_checkstring(L, 2);
    const char *sep = luaL_optstring(L, 3, ".");
    const char *rep = luaL_optstring(L, 4, LUA_DIRSEP);
    if (search(L, name, path, sep, rep)) {
        lua_remove(L, -2);                              // the contents; the path stays on top
        return 1;
    }
    lua_pushnil(L);
    lua_insert(L, -2);
    return 2;
}

static void setup_package(lua_State *L) {
    lua_getglobal(L, LUA_LOADLIBNAME);
    lua_pushnil(L);
    lua_setfield(L, -2, "loadlib");                     // no C modules
    lua_pushcfunction(L, l_searchpath);
    lua_setfield(L, -2, "searchpath");
    lua_getfield(L, -1, "searchers");
    lua_createtable(L, 2, 0);
    lua_rawgeti(L, -2, 1);                              // keep package.preload's searcher
    lua_rawseti(L, -2, 1);
    lua_pushvalue(L, -3);
    lua_pushcclosure(L, searcher_sd, 1);
    lua_rawseti(L, -2, 2);
    lua_setfield(L, -3, "searchers");
    lua_pop(L, 2);                                      // old searchers, package
}

static int open_sos(lua_State *L) {
    luaL_newlib(L, sos_lib);
    return 1;
}

static void open_libs(lua_State *L) {
    static const luaL_Reg libs[] = {
        {LUA_GNAME, luaopen_base},          {LUA_COLIBNAME, luaopen_coroutine},
        {LUA_TABLIBNAME, luaopen_table},    {LUA_STRLIBNAME, luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},    {LUA_UTF8LIBNAME, luaopen_utf8},
        {LUA_LOADLIBNAME, luaopen_package}, {"sos", open_sos},
        {NULL, NULL},
    };
    for (const luaL_Reg *lib = libs; lib->func; lib++) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }
    setup_package(L);
    lua_register(L, "print", l_print);
    lua_getglobal(L, "setmetatable");
    lua_pushcclosure(L, l_setmetatable, 1);
    lua_setglobal(L, "setmetatable");
    lua_getglobal(L, "xpcall");
    lua_pushcclosure(L, l_xpcall, 1);
    lua_setglobal(L, "xpcall");
    // These read files with C stdio, which isn't the SD card: use sos.read() + load().
    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");
}

// --- running a job ---------------------------------------------------------------------------

// Load the script: inline code, or the file at lua_job.path (through core 0).
static int load_script(lua_State *L) {
    if (lua_job.code)
        return luaL_loadbufferx(L, lua_job.code, lua_job.code_len, "=(command line)", "t");
    lua_pushstring(L, lua_job.path);
    lua_pushcfunction(L, l_read);
    lua_insert(L, -2);
    if (lua_pcall(L, 1, 2, 0) != LUA_OK)
        return LUA_ERRFILE;
    if (lua_isnil(L, -2)) {
        lua_pushfstring(L, "%s: %s", lua_job.path, lua_tostring(L, -1));
        return LUA_ERRFILE;
    }
    lua_pop(L, 1);
    size_t len;
    const char *src = lua_tolstring(L, -1, &len);
    char name[LUA_PATH_MAX + 1];
    snprintf(name, sizeof(name), "@%s", lua_job.path);
    int r = luaL_loadbufferx(L, src, len, name, "t");   // text only: no precompiled chunks
    lua_remove(L, -2);
    return r;
}

// --- core 1's stack ----------------------------------------------------------------------------

#define STACK_PATTERN 0x5a5aa5a5u

static uint32_t *stack;                 // LUA_CORE1_STACK bytes from the heap; grows down to here

// Fill the unused part of the stack with a pattern, so lua_core1_stack_peak() can see how deep
// the script goes. Leaves a margin below the current frame for this function and interrupts.
static void __attribute__((noinline)) stack_paint(void) {
    uint32_t here;
    uint32_t *limit = (uint32_t *)(((uintptr_t)&here - 256) & ~3u);
    for (uint32_t *p = stack; p < limit; p++)
        *p = STACK_PATTERN;
}

size_t lua_core1_stack_peak(void) {
    if (!stack)
        return 0;
    const uint32_t *p = stack, *top = stack + LUA_CORE1_STACK / 4;
    while (p < top && *p == STACK_PATTERN)
        p++;
    return (size_t)((uintptr_t)top - (uintptr_t)p);
}

static void run_job(void) {
    uint64_t t0 = time_us_64();
    stack_paint();
    mem_used = mem_peak = 0;
    mem_limit = lua_job.mem_limit;
    lua_job.ok = false;
    lua_job.error[0] = '\0';

    lua_State *L = lua_newstate(l_alloc, NULL);
    if (!L) {
        snprintf(lua_job.error, sizeof(lua_job.error), "not enough memory to start");
    } else {
        main_L = L;
        open_libs(L);
        lua_sethook(L, hook, LUA_MASKCOUNT, HOOK_EVERY);
        lua_pushcfunction(L, traceback);            // stack index 1: message handler
        int r = load_script(L);
        if (r == LUA_OK)
            r = lua_pcall(L, 0, 0, 1);
        if (r == LUA_OK) {
            lua_job.ok = true;
        } else if (r == LUA_ERRERR) {
            // The message handler (traceback) failed. It's simple, so in practice this means
            // the error came within a few levels of the C nesting limit (LUAI_MAXCCALLS) and
            // the handler's own calls went over it.
            snprintf(lua_job.error, sizeof(lua_job.error),
                     "C stack overflow (pcall, callbacks, metamethods or coroutines nested too deep)");
        } else {
            const char *msg = lua_tostring(L, -1);
            snprintf(lua_job.error, sizeof(lua_job.error), "%s",
                     msg ? msg : "error (not a string)");
        }
        lua_close(L);
        main_L = NULL;
    }
    lua_job.mem_peak = mem_peak;
    lua_job.run_ms = (uint32_t)((time_us_64() - t0) / 1000);
}

static void core1_main(void) {
    flash_safe_execute_core_init();     // let core 0 pause this core while it writes flash
    for (;;) {
        while (lua_job.state != LJ_START)
            __wfe();
        __dmb();
        lua_job.state = LJ_RUNNING;
        run_job();
        __dmb();
        lua_job.state = LJ_DONE;
        __sev();
    }
}

bool lua_core1_start(void) {
    if (stack)
        return true;
    stack = malloc(LUA_CORE1_STACK);
    if (!stack)
        return false;
    multicore_launch_core1_with_stack(core1_main, stack, LUA_CORE1_STACK);
    return true;
}
