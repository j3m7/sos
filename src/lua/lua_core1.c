// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The Lua interpreter on core 1. Runs one script at a time (lua_job, set up by core 0) in a
// fresh Lua state with the safe standard libraries (base, string, table, math, utf8, coroutine;
// no io, os, package or debug) plus `sos`, SOS's own library. Everything that touches the
// system goes to core 0 as a request (lua_bridge.h); this file only computes and waits.

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "hardware/sync.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "core/heaptag.h"
#include "core/log.h"
#include "lua/lua_bridge.h"
#include "lua/lua_complete.h"
#include "lua/lua_core1.h"
#include "net/http.h"

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
        heap_release(HEAP_LUA, ptr);
        mem_used -= (uint32_t)osize;
        return NULL;
    }
    if (nsize > osize && mem_used - osize + nsize > mem_limit)
        return NULL;                            // Lua raises "not enough memory"
    void *p = heap_realloc(HEAP_LUA, ptr, nsize);
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

// sos.log(message [, level]): a line in the log (the user facility, tagged with the script), at
// "notice" unless a level ("err", "warning", "notice", "info", "debug") is given.
static int l_log(lua_State *L) {
    static const char *const levels[] = {"err", "warning", "notice", "info", "debug", NULL};
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    int level = SOS_LOG_ERR + luaL_checkoption(L, 2, "notice", levels);
    if (len >= LUA_REQ_DATA)
        len = LUA_REQ_DATA - 1;
    lua_req.type = LR_LOG;
    lua_req.offset = (uint32_t)level;
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

// --- sos.http ----------------------------------------------------------------------------------
//
// sos.http.request(opts), sos.http.get(url [, opts]), sos.http.post(url, body [, opts]): one
// HTTPS request (net/http.h), made by core 0. Returns a table {status, reason, headers, body},
// whatever the status, or nil and an error when there's no response (bad options, DNS, TLS,
// timeout, a body larger than max_body). The whole body is read into a string.

#define HTTP_LUA_HEADERS    32              // extra request headers
#define HTTP_LUA_BODY_MAX   (32 * 1024)     // default max_body: a script has 128 KB in all

#define HTTP_CLOSER "sos.http.closer"       // registry: metatable of the to-be-closed guard

static bool http_open;                      // core 0 holds a request whose body isn't all read

// A request answered "interrupted" because the script is being stopped: stop now.
static void check_cancel(lua_State *L) {
    if (lua_job.cancel)
        interrupt(L);
}

// __close of the guard that http_request() keeps in a to-be-closed slot: however the call ends
// (an error, "not enough memory" while the body grows, Ctrl-C, max_body), a request left
// unfinished is dropped, so it doesn't hold a TLS connection while the script goes on.
// (A coroutine that dies with an error isn't unwound until coroutine.close(); core 0 then drops
// the request with the next one, at the end of the prompt line, or when the script ends.)
static int http_closer(lua_State *L) {
    (void)L;
    if (http_open) {
        http_open = false;
        lua_req.type = LR_HTTP_CLOSE;
        request();
    }
    return 0;
}

// The string field `name` of table t, or NULL if it's nil. It stays on the stack (which keeps
// it alive for core 0 to read). No NUL bytes: they'd cut the text short.
static const char *opt_string(lua_State *L, int t, const char *name) {
    if (lua_getfield(L, t, name) == LUA_TNIL)
        return NULL;
    if (lua_type(L, -1) != LUA_TSTRING)
        luaL_error(L, "'%s' must be a string", name);
    size_t len;
    const char *s = lua_tolstring(L, -1, &len);
    if (strlen(s) != len && strcmp(name, "body"))
        luaL_error(L, "'%s' contains a NUL byte", name);
    return s;
}

static lua_Integer opt_integer(lua_State *L, int t, const char *name, lua_Integer def,
                               lua_Integer min, lua_Integer max) {
    lua_getfield(L, t, name);
    lua_Integer v = def;
    if (!lua_isnil(L, -1)) {
        int ok;
        v = lua_tointegerx(L, -1, &ok);
        if (!ok || v < min || v > max)
            luaL_error(L, "'%s' must be an integer from %I to %I", name, min, max);
    }
    lua_pop(L, 1);
    return v;
}

// Turn the headers table at index h into "Name: value" strings, kept in a new table pushed on
// the stack, with pointers to them in lines[] (NULL-terminated). A Content-Type header becomes
// *content_type, since the client sends that one itself.
static void header_lines(lua_State *L, int h, const char **lines, const char **content_type) {
    lua_newtable(L);
    int keep = lua_gettop(L), n = 0;
    lua_pushnil(L);
    while (lua_next(L, h)) {                    // key at -2, value at -1
        size_t klen, vlen;
        if (lua_type(L, -2) != LUA_TSTRING || (lua_type(L, -1) != LUA_TSTRING &&
                                               lua_type(L, -1) != LUA_TNUMBER))
            luaL_error(L, "headers must map names to strings");
        lua_pushvalue(L, -1);                   // a copy: tostring on a number changes it
        const char *k = lua_tolstring(L, -3, &klen);
        const char *v = lua_tolstring(L, -1, &vlen);
        if (strlen(k) != klen || strlen(v) != vlen)
            luaL_error(L, "header '%s' contains a NUL byte", k);
        if (!strcasecmp(k, "content-type")) {
            *content_type = v;
            lua_setfield(L, keep, "content-type");
        } else {
            if (n == HTTP_LUA_HEADERS)
                luaL_error(L, "more than %d headers", HTTP_LUA_HEADERS);
            lines[n] = lua_pushfstring(L, "%s: %s", k, v);
            lua_rawseti(L, keep, ++n);
            lua_pop(L, 1);                      // the copy
        }
        lua_pop(L, 1);                          // the value; the key stays for lua_next
    }
    lines[n] = NULL;
}

// The headers from the answer ("name:value\n" each) as a table; a repeated header's values are
// joined with ", ".
static void push_headers(lua_State *L) {
    lua_newtable(L);
    const char *s = (const char *)lua_req.data, *end = s + lua_req.len;
    while (s < end) {
        const char *nl = memchr(s, '\n', (size_t)(end - s));
        const char *colon = memchr(s, ':', (size_t)(nl - s));
        if (!nl || !colon)
            break;
        lua_pushlstring(L, s, (size_t)(colon - s));
        lua_pushvalue(L, -1);
        if (lua_rawget(L, -3) == LUA_TSTRING) {
            lua_pushliteral(L, ", ");
            lua_pushlstring(L, colon + 1, (size_t)(nl - colon - 1));
            lua_concat(L, 3);
        } else {
            lua_pop(L, 1);
            lua_pushlstring(L, colon + 1, (size_t)(nl - colon - 1));
        }
        lua_rawset(L, -3);
        s = nl + 1;
    }
}

// The request described by the options table at index t.
static int http_request(lua_State *L, int t) {
    luaL_checkstack(L, 16, NULL);
    const char *url = opt_string(L, t, "url");
    if (!url)
        return luaL_error(L, "'url' is required");
    size_t body_len = 0;
    const char *body = opt_string(L, t, "body");
    if (body)
        body_len = lua_rawlen(L, -1);
    const char *method = opt_string(L, t, "method");
    if (!method)
        method = body ? "POST" : "GET";
    const char *content_type = opt_string(L, t, "content_type");
    const char *lines[HTTP_LUA_HEADERS + 1] = {NULL};
    const char *header_ct = NULL;
    if (lua_getfield(L, t, "headers") == LUA_TTABLE)
        header_lines(L, lua_gettop(L), lines, &header_ct);
    else if (!lua_isnil(L, -1))
        return luaL_error(L, "'headers' must be a table");
    if (!content_type)
        content_type = header_ct;
    lua_Integer timeout = opt_integer(L, t, "timeout", 0, 1, 600000);
    lua_Integer max_body = opt_integer(L, t, "max_body", HTTP_LUA_BODY_MAX, 0, 0x7fffffff);

    http_opts_t o = {
        .method = method,
        .url = url,
        .content_type = content_type,
        .headers = lines,
        .body = body,
        .body_len = body_len,
        .timeout_ms = (uint32_t)timeout,
    };
    lua_req.type = LR_HTTP_REQUEST;
    lua_req.opts = &o;
    request();
    lua_req.opts = NULL;
    check_cancel(L);
    if (lua_req.result < 0)
        return fail(L);
    http_open = true;
    lua_newtable(L);                            // the guard (see http_closer)
    luaL_setmetatable(L, HTTP_CLOSER);
    lua_toclose(L, -1);

    lua_createtable(L, 0, 4);
    lua_pushinteger(L, (lua_Integer)lua_req.result);
    lua_setfield(L, -2, "status");
    lua_pushstring(L, lua_req.path);
    lua_setfield(L, -2, "reason");
    push_headers(L);
    lua_setfield(L, -2, "headers");

    luaL_Buffer b;
    luaL_buffinit(L, &b);
    size_t total = 0;
    for (;;) {
        lua_req.type = LR_HTTP_READ;
        request();
        check_cancel(L);
        if (lua_req.result < 0) {               // core 0 has dropped the request
            http_open = false;
            luaL_pushresult(&b);
            return fail(L);
        }
        if (lua_req.len == 0) {                 // the end: core 0 has dropped it
            http_open = false;
            break;
        }
        if (total + lua_req.len > (size_t)max_body) {   // the guard closes it
            luaL_pushresult(&b);
            lua_pushnil(L);
            lua_pushfstring(L, "response body larger than %I bytes (max_body)", max_body);
            return 2;
        }
        luaL_addlstring(&b, (const char *)lua_req.data, lua_req.len);
        total += lua_req.len;
    }
    luaL_pushresult(&b);
    lua_setfield(L, -2, "body");
    return 1;
}

static int l_http_request(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    return http_request(L, 1);
}

// A copy of the options table at index opts (or a new one), with method and url set; pushed.
static int with_options(lua_State *L, int opts, const char *method, int url) {
    luaL_checkstring(L, url);
    if (!lua_isnoneornil(L, opts))
        luaL_checktype(L, opts, LUA_TTABLE);
    lua_newtable(L);
    int t = lua_gettop(L);
    if (!lua_isnoneornil(L, opts)) {
        lua_pushnil(L);
        while (lua_next(L, opts)) {
            lua_pushvalue(L, -2);
            lua_insert(L, -2);
            lua_rawset(L, t);
        }
    }
    lua_pushstring(L, method);
    lua_setfield(L, t, "method");
    lua_pushvalue(L, url);
    lua_setfield(L, t, "url");
    return t;
}

static int l_http_get(lua_State *L) {
    return http_request(L, with_options(L, 2, "GET", 1));
}

static int l_http_post(lua_State *L) {
    luaL_checkstring(L, 2);
    int t = with_options(L, 3, "POST", 1);
    lua_pushvalue(L, 2);
    lua_setfield(L, t, "body");
    return http_request(L, t);
}

static const luaL_Reg http_lib[] = {
    {"request", l_http_request}, {"get", l_http_get}, {"post", l_http_post}, {NULL, NULL},
};

// --- timers: sos.every, sos.after, sos.cancel ---------------------------------------------------
//
// Timers live in a registry table, id -> {fn = function, due = time_us_64() deadline, every = period
// in microseconds or 0}. They run once the script's main chunk has returned (run_timers): the script
// then keeps going until no timers are left, it's stopped, or a callback raises an error.

#define TIMERS_KEY "sos.timers"
#define TIMER_MIN_US 10000                      // shortest interval: 0.01 s

static lua_Integer next_timer_id;

static int add_timer(lua_State *L, bool repeat) {
    lua_Number sec = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (lua_job.repl)
        return luaL_error(L, "timers don't run at the interactive prompt");
    if (!(sec >= 0) || sec > 31536000)          // also NaN; at most a year
        return luaL_argerror(L, 1, "seconds out of range");
    lua_Integer us = (lua_Integer)(sec * 1e6);
    if (repeat && us < TIMER_MIN_US)
        return luaL_argerror(L, 1, "interval below 0.01 s");
    lua_getfield(L, LUA_REGISTRYINDEX, TIMERS_KEY);
    lua_createtable(L, 0, 3);
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, "fn");
    lua_pushinteger(L, (lua_Integer)time_us_64() + us);
    lua_setfield(L, -2, "due");
    lua_pushinteger(L, repeat ? us : 0);
    lua_setfield(L, -2, "every");
    lua_rawseti(L, -2, ++next_timer_id);
    lua_pushinteger(L, next_timer_id);
    return 1;
}

static int l_every(lua_State *L) { return add_timer(L, true); }
static int l_after(lua_State *L) { return add_timer(L, false); }

static int l_cancel(lua_State *L) {
    lua_Integer id = luaL_checkinteger(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, TIMERS_KEY);
    bool found = lua_rawgeti(L, -1, id) != LUA_TNIL;
    lua_pop(L, 1);
    lua_pushnil(L);
    lua_rawseti(L, -2, id);
    lua_pushboolean(L, found);
    return 1;
}

// After the main chunk: run timers until none are left. Returns LUA_OK, or an error status with
// the message on the stack (a callback failed, or the script was stopped). Stack index 1 is the
// traceback handler.
static int run_timers(lua_State *L) {
    for (;;) {
        lua_getfield(L, LUA_REGISTRYINDEX, TIMERS_KEY);
        lua_Integer id = 0, due = 0;
        lua_pushnil(L);
        while (lua_next(L, -2)) {                       // the earliest one (lowest id on a tie)
            lua_getfield(L, -1, "due");
            lua_Integer d = lua_tointeger(L, -1);
            lua_Integer k = lua_tointeger(L, -3);
            lua_pop(L, 2);
            if (!id || d < due || (d == due && k < id)) {
                id = k;
                due = d;
            }
        }
        if (!id) {
            lua_pop(L, 1);
            return LUA_OK;                              // none left: the script ends normally
        }
        for (;;) {                                      // wait for it
            if (lua_job.cancel) {
                lua_pop(L, 1);
                lua_pushliteral(L, "interrupted");
                return LUA_ERRRUN;
            }
            lua_Integer left = due - (lua_Integer)time_us_64();
            if (left <= 0)
                break;
            sleep_us(left < 5000 ? (uint64_t)left : 5000);
        }
        lua_rawgeti(L, -1, id);
        lua_getfield(L, -1, "every");
        lua_Integer every = lua_tointeger(L, -1);
        lua_pop(L, 1);
        if (every) {                                    // next time; skip runs missed meanwhile
            lua_Integer next = due + every, now = (lua_Integer)time_us_64();
            lua_pushinteger(L, next > now ? next : now + every);
            lua_setfield(L, -2, "due");
        } else {
            lua_pushnil(L);
            lua_rawseti(L, -3, id);                     // one-shot: gone before it runs
        }
        lua_getfield(L, -1, "fn");
        lua_remove(L, -2);                              // the timer
        lua_remove(L, -2);                              // the table
        lua_pushinteger(L, id);                         // the callback gets its id
        int r = lua_pcall(L, 1, 0, 1);
        if (r != LUA_OK)
            return r;
    }
}

static const luaL_Reg sos_lib[] = {
    {"log", l_log},       {"time", l_time},     {"uptime", l_uptime}, {"sleep", l_sleep},
    {"read", l_read},     {"write", l_write},   {"append", l_append}, {"list", l_list},
    {"remove", l_remove}, {"mkdir", l_mkdir},   {"every", l_every},   {"after", l_after},
    {"cancel", l_cancel}, {NULL, NULL},
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
    luaL_newlib(L, http_lib);
    lua_setfield(L, -2, "http");
    luaL_newmetatable(L, HTTP_CLOSER);
    lua_pushcfunction(L, http_closer);
    lua_setfield(L, -2, "__close");
    lua_pop(L, 1);
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

// --- the interactive prompt (`lua` alone) ------------------------------------------------------
//
// One Lua state for the whole session. Each line comes from core 0 (lua_job.repl_line). As in the
// standard `lua` program: a first line is tried as an expression ("return <line>") so its value
// is printed; a statement that isn't finished yet (a syntax error at "<eof>") waits for more
// lines; errors are printed and the session goes on.

// Print what a line returned (the values from `base` + 1 up), like print().
static void repl_print(lua_State *L, int base) {
    int n = lua_gettop(L) - base;
    if (n <= 0)
        return;
    lua_pushcfunction(L, l_print);
    lua_insert(L, base + 1);
    if (lua_pcall(L, n, 0, 0) != LUA_OK) {          // a __tostring that fails, say
        const char *msg = lua_tostring(L, -1);
        sos_lua_write("error printing the result: ", 27);
        sos_lua_write(msg ? msg : "?", msg ? strlen(msg) : 1);
        sos_lua_write("\n", 1);
    }
}

static void repl_error(lua_State *L) {
    // While stopping (Ctrl-C), the "interrupted" message and its traceback aren't news.
    if (!lua_job.cancel) {
        size_t len;
        const char *msg = lua_tolstring(L, -1, &len);
        if (!msg)
            msg = "(error object is not a string)", len = strlen(msg);
        sos_lua_write(msg, len);
        sos_lua_write("\n", 1);
    }
    lua_pop(L, 1);
}

// Run one line. Stack: 1 = traceback handler, 2 = the unfinished statement so far ("" if none).
// Returns true if the statement continues on the next line.
static bool repl_line(lua_State *L, const char *line) {
    size_t plen;
    lua_tolstring(L, 2, &plen);
    int status;
    if (plen == 0) {                                // try it as an expression first
        const char *ret = lua_pushfstring(L, "return %s;", line);
        status = luaL_loadbufferx(L, ret, strlen(ret), "=stdin", "t");
        if (status == LUA_OK) {
            lua_remove(L, -2);                      // the "return ..." text
            goto run;
        }
        lua_pop(L, 2);
        lua_pushstring(L, line);
    } else {
        lua_pushfstring(L, "%s\n%s", lua_tostring(L, 2), line);
    }
    size_t len;
    const char *src = lua_tolstring(L, 3, &len);
    status = luaL_loadbufferx(L, src, len, "=stdin", "t");
    if (status == LUA_ERRSYNTAX) {
        size_t mlen;
        const char *msg = lua_tolstring(L, -1, &mlen);
        if (mlen >= 5 && !strcmp(msg + mlen - 5, "<eof>")) {     // not finished yet
            lua_pop(L, 1);
            lua_replace(L, 2);                      // keep the text for the next line
            return true;
        }
    }
    lua_remove(L, 3);                               // the text
    lua_pushliteral(L, "");
    lua_replace(L, 2);                              // nothing unfinished any more
    if (status != LUA_OK) {
        repl_error(L);
        return false;
    }
run:
    {
        int base = lua_gettop(L) - 1;
        if (lua_pcall(L, 0, LUA_MULTRET, 1) != LUA_OK)
            repl_error(L);
        else
            repl_print(L, base);
        lua_settop(L, 2);
    }
    return false;
}

static void run_repl(lua_State *L) {
    lua_pushliteral(L, "");                         // 2: unfinished statement
    for (;;) {
        while (!lua_job.repl_cmd && !lua_job.repl_quit)
            __wfe();
        __dmb();
        if (lua_job.repl_quit)
            break;
        if (lua_job.repl_complete) {                // Tab: names, not a line (no code runs)
            size_t used;
            bool more;
            lua_job.repl_names = lua_complete(L, lua_job.repl_line, (char *)lua_req.data,
                                              LUA_REQ_DATA, &used, &more);
            lua_job.repl_names_len = (uint32_t)used;
            lua_settop(L, 2);
            __dmb();
            lua_job.repl_cmd = false;
            __sev();
            continue;
        }
        // A Ctrl-C during the last line left the hook running every instruction (interrupt()).
        lua_sethook(L, hook, LUA_MASKCOUNT, HOOK_EVERY);
        if (lua_job.repl_reset) {
            lua_pushliteral(L, "");
            lua_replace(L, 2);
        }
        lua_job.repl_incomplete = repl_line(L, lua_job.repl_line);
        lua_settop(L, 2);
        __dmb();
        lua_job.repl_cmd = false;
        __sev();
    }
    lua_job.ok = true;
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
        lua_newtable(L);
        lua_setfield(L, LUA_REGISTRYINDEX, TIMERS_KEY);
        next_timer_id = 0;
        http_open = false;
        lua_sethook(L, hook, LUA_MASKCOUNT, HOOK_EVERY);
        lua_pushcfunction(L, traceback);            // stack index 1: message handler
        if (lua_job.repl) {
            run_repl(L);
            lua_close(L);
            main_L = NULL;
            goto done;
        }
        int r = load_script(L);
        if (r == LUA_OK)
            r = lua_pcall(L, 0, 0, 1);
        if (r == LUA_OK)
            r = run_timers(L);
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
done:
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
    stack = heap_alloc(HEAP_LUA, LUA_CORE1_STACK);
    if (!stack)
        return false;
    multicore_launch_core1_with_stack(core1_main, stack, LUA_CORE1_STACK);
    return true;
}
