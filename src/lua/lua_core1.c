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

// First: the Lua headers keep the definitions it makes for them (as for the Lua sources).
#include "lua/lua_port.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include "core/heaptag.h"
#include "core/log.h"
#include "files/path.h"
#include "lua/lua_bridge.h"
#include "lua/lua_complete.h"
#include "lua/lua_codec.h"
#include "lua/lua_core1.h"
#include "lua/lua_date.h"
#include "lua/lua_hash.h"
#include "lua/lua_json.h"
#include "lua/lua_health.h"
#include "lua/lua_info.h"
#if SOS_HAVE_HTTP
#include "net/http.h"
#endif

#define HOOK_EVERY 1000             // instructions between cancel checks

static uint32_t mem_used, mem_peak, mem_limit;

// --- talking to core 0 -----------------------------------------------------------------------

// Send the filled-in lua_req and wait for core 0's answer. Core 0 always answers, so this
// doesn't check for cancellation (the hook does, between instructions).
static void request(void) {
    lua_req.done = false;
    __dmb();
    lua_beat.waiting = LUA_WAIT_CORE0;
    lua_req.pending = true;
    __sev();
    while (!lua_req.done)
        __wfe();
    __dmb();
    lua_req.pending = false;
    lua_beat.waiting = LUA_WAIT_NONE;
}

void sos_lua_write(const char *s, size_t len) {
    while (len) {
        uint32_t used = lua_out.head - lua_out.tail;
        if (used == LUA_OUT_SIZE) {             // full: core 0 drains (or discards) it
            if (lua_job.cancel) {
                lua_beat.waiting = LUA_WAIT_NONE;
                return;
            }
            lua_beat.waiting = LUA_WAIT_OUTPUT;
            __wfe();
            continue;
        }
        lua_beat.waiting = LUA_WAIT_NONE;
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
    lua_beat.beats++;                       // the `core1` health check's heartbeat
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
// A script's path as core 0 takes it, into out (LUA_PATH_MAX bytes). Absolute: as given.
// Relative: cwd + "/" + path (cwd "/" needs no extra separator). Core 0 normalizes "." and "..".
static void resolve_path(lua_State *L, const char *p, char *out) {
    const char *dir = p[0] == '/' ? "" : lua_job.cwd;
    const char *sep = (p[0] == '/' || !strcmp(dir, "/")) ? "" : "/";
    size_t need = strlen(dir) + strlen(sep) + strlen(p) + 1;
    if (need > LUA_PATH_MAX)
        luaL_error(L, "path too long: %s", p);
    memcpy(out, dir, strlen(dir));
    memcpy(out + strlen(dir), sep, strlen(sep));
    memcpy(out + strlen(dir) + strlen(sep), p, strlen(p) + 1);
}

static void set_req_path(lua_State *L, const char *p) {
    resolve_path(L, p, lua_req.path);
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

// sos.reboot([why]): restart the board (refused in the first LUA_REBOOT_MIN_S after boot). Returns
// only when refused: nil and why.
static int l_reboot(lua_State *L) {
    size_t len = 0;
    const char *why = luaL_optlstring(L, 1, "", &len);
    if (len > LUA_REBOOT_WHY_MAX)
        len = LUA_REBOOT_WHY_MAX;
    lua_req.type = LR_REBOOT;
    memcpy(lua_req.data, why, len);
    lua_req.data[len] = '\0';
    request();
    if (lua_job.cancel)
        return interrupt(L);
    return fail(L);
}

// The zone the script started with (lua_job.tz_rule), parsed into *r: r, or NULL for UTC (none
// set, or a saved rule that doesn't parse, which `time` reports).
static const tzrule_t *zone(tzrule_t *r) {
    return lua_job.tz_name[0] && tzrule_parse(lua_job.tz_rule, r, NULL) ? r : NULL;
}

// The time now into *t; false if the clock isn't set (lua_req.error says so).
static bool now(int64_t *t) {
    lua_req.type = LR_TIME;
    request();
    *t = lua_req.result;
    return lua_req.result > 0;
}

// sos.time(): seconds since 1970 (UTC), or nil and why if the clock isn't set. sos.time(table):
// that date and time (UTC) in seconds, as os.time() (lua_date.c; no clock needed).
// sos.localtime(table): the same with the table read as local time in the board's zone.
static int time_of(lua_State *L, bool local) {
    tzrule_t r;
    if (local || !lua_isnoneornil(L, 1)) {
        lua_pushinteger(L, (lua_Integer)lua_date_from_table(L, 1, local ? zone(&r) : NULL));
        return 1;
    }
    int64_t t;
    if (!now(&t))
        return fail(L);
    lua_pushinteger(L, (lua_Integer)t);
    return 1;
}

static int l_time(lua_State *L) { return time_of(L, false); }
static int l_localtime(lua_State *L) { return time_of(L, true); }

// sos.date([format [, t]]): like os.date, in UTC (lua_date.c). Without t, the time now: nil and
// why if the clock isn't set. sos.localdate: the same in the board's zone ("!" still means UTC).
static int date_of(lua_State *L, bool local) {
    const char *fmt = luaL_optstring(L, 1, LUA_DATE_DEFAULT);
    int64_t t;
    if (lua_isnoneornil(L, 2)) {
        if (!now(&t))
            return fail(L);
    } else {
        t = (int64_t)luaL_checkinteger(L, 2);
    }
    tzrule_t r;
    lua_date_push(L, fmt, t, local ? zone(&r) : NULL);
    return 1;
}

static int l_date(lua_State *L) { return date_of(L, false); }
static int l_localdate(lua_State *L) { return date_of(L, true); }

// sos.zone(): the board's time zone as the script started with it: its name and POSIX rule, or
// "UTC" and nil when none is set (or its saved rule isn't usable).
static int l_zone(lua_State *L) {
    tzrule_t r;
    if (!zone(&r)) {
        lua_pushliteral(L, "UTC");
        lua_pushnil(L);
        return 2;
    }
    lua_pushstring(L, lua_job.tz_name);
    lua_pushstring(L, lua_job.tz_rule);
    return 2;
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
        lua_beat.beats++;
        sleep_ms(5);
    }
    return 0;
}

#ifdef SOS_TEST_COMMANDS
// sos.spin(ms), test builds only: a C loop on core 1 that runs no Lua instruction, so neither the
// hook (Ctrl-C, `lua stop`) nor the `core1` health check's heartbeat sees it until it returns. For
// the board run of docs/design/health.md §9.5. At most two minutes.
static int l_spin(lua_State *L) {
    lua_Integer ms = luaL_checkinteger(L, 1);
    if (ms < 0 || ms > 120000)
        return luaL_error(L, "spin: 0 to 120000 ms");
    uint64_t until = time_us_64() + (uint64_t)ms * 1000;
    while (time_us_64() < until)
        tight_loop_contents();
    return 0;
}
#endif

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

// sos.rename(from, to [, replace]): to must not exist, unless replace is true and it's a file.
static bool rename_to(const char *from, const char *to, bool replace) {
    memcpy(lua_req.path, from, strlen(from) + 1);
    memcpy(lua_req.data, to, strlen(to) + 1);
    lua_req.type = LR_RENAME;
    lua_req.offset = replace ? 1 : 0;
    request();
    return lua_req.result >= 0;
}

static int l_rename(lua_State *L) {
    char from[LUA_PATH_MAX], to[LUA_PATH_MAX];
    resolve_path(L, luaL_checkstring(L, 1), from);
    resolve_path(L, luaL_checkstring(L, 2), to);
    if (!rename_to(from, to, lua_toboolean(L, 3)))
        return fail(L);
    lua_pushboolean(L, 1);
    return 1;
}

// One request to remove `path`, its result ignored (cleaning up after a failure).
static void remove_quietly(const char *path) {
    memcpy(lua_req.path, path, strlen(path) + 1);
    lua_req.type = LR_REMOVE;
    request();
}

// sos.copy(from, to): the file's bytes copied, 4 KB a request each way; returns the size. to is
// created or replaced. A failure after part of it was written removes the copy.
static int l_copy(lua_State *L) {
    char from[LUA_PATH_MAX], to[LUA_PATH_MAX], err[LUA_PATH_MAX + sizeof(lua_req.error) + 2] = "";
    resolve_path(L, luaL_checkstring(L, 1), from);
    resolve_path(L, luaL_checkstring(L, 2), to);
    // The same file under another spelling ("/a/../x" and "/x", or another case) would be
    // truncated by the first write: compare them as core 0 will resolve them.
    char nfrom[LUA_PATH_MAX], nto[LUA_PATH_MAX];
    if (!path_resolve("/", from, nfrom, sizeof(nfrom)) || !path_resolve("/", to, nto, sizeof(nto)))
        return luaL_error(L, "path too long");
    if (!strcasecmp(nfrom, nto))                // the card's names ignore case
        return luaL_error(L, "source and destination are the same");
    uint64_t done = 0;
    for (;;) {
        if (lua_job.cancel)
            return interrupt(L);                // Ctrl-C: what was copied stays
        memcpy(lua_req.path, from, strlen(from) + 1);
        lua_req.type = LR_READ;
        lua_req.offset = (uint32_t)done;
        request();
        if (lua_req.result < 0) {
            snprintf(err, sizeof(err), "%s: %s", luaL_checkstring(L, 1), lua_req.error);
            break;
        }
        if (lua_req.len == 0 && done > 0)
            break;                              // the end (an empty file still gets a write)
        memcpy(lua_req.path, to, strlen(to) + 1);
        lua_req.type = LR_WRITE;
        lua_req.offset = done ? 1 : 0;
        uint32_t n = lua_req.len;
        request();
        if (lua_req.result < 0) {
            snprintf(err, sizeof(err), "%s: %s", luaL_checkstring(L, 2), lua_req.error);
            break;
        }
        done += n;
        if (n == 0)
            break;                              // an empty file: copied
        if (done > 0xffffffffu - LUA_REQ_DATA) {
            snprintf(err, sizeof(err), "file too large to copy (4 GB)");
            break;
        }
    }
    if (lua_req.result < 0 || err[0]) {
        if (done > 0)
            remove_quietly(to);
        lua_pushnil(L);
        lua_pushstring(L, err);
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)done);
    return 1;
}

// sos.stat(path): {size=, dir=, time=}, or nil and why ("no such file" if nothing is there).
static int l_stat(lua_State *L) {
    set_path(L, 1);
    lua_req.type = LR_STAT;
    request();
    if (lua_req.result < 0)
        return fail(L);
    lua_stat_t st;
    memcpy(&st, lua_req.data, sizeof(st));
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, (lua_Integer)st.size);
    lua_setfield(L, -2, "size");
    lua_pushboolean(L, st.dir);
    lua_setfield(L, -2, "dir");
    if (st.time) {
        lua_pushinteger(L, (lua_Integer)st.time);
        lua_setfield(L, -2, "time");
    }
    return 1;
}

// sos.exists(path): true or false; nil and why if it can't tell (no card).
static int l_exists(lua_State *L) {
    set_path(L, 1);
    lua_req.type = LR_STAT;
    request();
    if (lua_req.result == LUA_REQ_NOT_FOUND) {
        lua_pushboolean(L, 0);
        return 1;
    }
    if (lua_req.result < 0)
        return fail(L);
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

// A request answered "interrupted" because the script is being stopped: stop now.
static void check_cancel(lua_State *L) {
    if (lua_job.cancel)
        interrupt(L);
}

#if SOS_HAVE_HTTP
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

static void set_int(lua_State *L, const char *name, lua_Integer v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, name);
}

// The timings from the answer that ended the body, as fields of the response table on top.
static void push_stats(lua_State *L, const lua_http_stats_t *s) {
    set_int(L, "redirects", s->redirects);
    set_int(L, "headers_ms", s->headers_ms);
    set_int(L, "elapsed_ms", s->elapsed_ms);
    if (!s->tls)
        return;
    lua_createtable(L, 0, 4);
    set_int(L, "handshake_ms", s->handshake_us / 1000);
    set_int(L, "longest_step_us", s->longest_step_us);
    set_int(L, "heap_peak", s->heap_peak);
    lua_pushboolean(L, s->dates_unchecked);
    lua_setfield(L, -2, "dates_unchecked");
    if (s->cert) {
        lua_createtable(L, 0, 4);
        lua_pushstring(L, s->cert_subject);
        lua_setfield(L, -2, "subject");
        lua_pushstring(L, s->cert_issuer);
        lua_setfield(L, -2, "issuer");
        if (s->cert_not_before) {
            lua_pushinteger(L, (lua_Integer)s->cert_not_before);
            lua_setfield(L, -2, "not_before");
        }
        if (s->cert_not_after) {
            lua_pushinteger(L, (lua_Integer)s->cert_not_after);
            lua_setfield(L, -2, "not_after");
        }
        lua_setfield(L, -2, "cert");
    }
    lua_setfield(L, -2, "tls");
}

// The request described by the options table at index t.
// sos.http.download's file: one piece of the body, already in lua_req.data (lua_req.len bytes),
// written to it (the first piece creates or truncates it). False if the write failed.
static bool file_piece(const char *file, bool first) {
    memcpy(lua_req.path, file, strlen(file) + 1);
    lua_req.type = LR_WRITE;
    lua_req.offset = first ? 0 : 1;
    request();
    return lua_req.result >= 0;
}

// A download failed: remove the file if this download wrote to it (`wrote`; a file that was there
// before and never touched stays as it was), and return nil and `why`.
static int drop_file(lua_State *L, const char *file, bool wrote, const char *why) {
    char err[sizeof(lua_req.error)];
    snprintf(err, sizeof(err), "%s", why);
    if (wrote) {
        memcpy(lua_req.path, file, strlen(file) + 1);
        lua_req.type = LR_REMOVE;
        request();
    }
    lua_pushnil(L);
    lua_pushstring(L, err);
    return 2;
}

// The request described by the options table at index t. With `file` (an absolute path), a 2xx
// response's body goes to that file instead of into the response, which has `size` instead of
// `body`; another status's body comes back as usual, and the file isn't touched.
static int http_request(lua_State *L, int t, const char *file) {
    luaL_checkstack(L, 16, NULL);
    const char *url = opt_string(L, t, "url");
    if (!url)
        return luaL_error(L, "'url' is required");
    size_t body_len = 0;
    const char *body = opt_string(L, t, "body");
    if (body)
        lua_tolstring(L, -1, &body_len);
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
    // -1: not given. Then a body in memory stops at HTTP_LUA_BODY_MAX, and a file has no limit.
    lua_Integer max_body = opt_integer(L, t, "max_body", -1, 0, 0x7fffffff);
    // Off unless asked: GET and HEAD only, without a body or headers (http_start() refuses).
    lua_Integer redirects = opt_integer(L, t, "redirects", 0, 0, HTTP_REDIRECTS_MAX);

    http_opts_t o = {
        .method = method,
        .url = url,
        .content_type = content_type,
        .headers = lines,
        .body = body,
        .body_len = body_len,
        .timeout_ms = (uint32_t)timeout,
        .max_redirects = (uint8_t)redirects,
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

    int64_t status = lua_req.result;
    lua_createtable(L, 0, 7);
    lua_pushinteger(L, (lua_Integer)lua_req.result);
    lua_setfield(L, -2, "status");
    lua_pushstring(L, lua_req.path);
    lua_setfield(L, -2, "reason");
    push_headers(L);
    lua_setfield(L, -2, "headers");

    bool to_file = file && status >= 200 && status < 300;
    lua_Integer limit = max_body >= 0 ? max_body : to_file ? 0x7fffffff : HTTP_LUA_BODY_MAX;
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    size_t total = 0;
    lua_http_stats_t stats;
    for (;;) {
        lua_req.type = LR_HTTP_READ;
        request();
        check_cancel(L);
        if (lua_req.result < 0) {               // core 0 has dropped the request
            http_open = false;
            luaL_pushresult(&b);
            return to_file ? drop_file(L, file, total > 0, lua_req.error) : fail(L);
        }
        if (lua_req.len == 0) {                 // the end: core 0 has dropped it
            http_open = false;
            memcpy(&stats, lua_req.data, sizeof(stats));
            break;
        }
        if (total + lua_req.len > (size_t)limit) {      // the guard closes it
            luaL_pushresult(&b);
            const char *why = lua_pushfstring(L, "response body larger than %I bytes (max_body)",
                                              limit);
            if (to_file)
                return drop_file(L, file, total > 0, why);
            lua_pushnil(L);
            lua_insert(L, -2);
            return 2;
        }
        if (to_file) {
            if (!file_piece(file, total == 0)) {
                luaL_pushresult(&b);
                return drop_file(L, file, total > 0, lua_req.error);
            }
        } else {
            luaL_addlstring(&b, (const char *)lua_req.data, lua_req.len);
        }
        total += lua_req.len;
    }
    luaL_pushresult(&b);
    lua_pushstring(L, (const char *)lua_req.data + sizeof(stats));     // the end's last URL
    lua_setfield(L, -3, "url");
    if (to_file) {
        lua_pop(L, 1);
        lua_req.len = 0;
        if (total == 0 && !file_piece(file, true))      // an empty body: an empty file
            return fail(L);
        lua_pushinteger(L, (lua_Integer)total);
        lua_setfield(L, -2, "size");
    } else {
        lua_setfield(L, -2, "body");
    }
    push_stats(L, &stats);
    return 1;
}

static int l_http_request(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    return http_request(L, 1, NULL);
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
    return http_request(L, with_options(L, 2, "GET", 1), NULL);
}

static int l_http_post(lua_State *L) {
    luaL_checkstring(L, 2);
    int t = with_options(L, 3, "POST", 1);
    lua_pushvalue(L, 2);
    lua_setfield(L, t, "body");
    return http_request(L, t, NULL);
}

static int l_http_head(lua_State *L) {
    return http_request(L, with_options(L, 2, "HEAD", 1), NULL);
}

// sos.http.download(url, path [, opts]): a GET whose 2xx body goes to the file at path.
// The body goes to "<path>.part", renamed over path once it's all there, so a download that
// fails (or is stopped) never touches a file already at path.
static int l_http_download(lua_State *L) {
    luaL_checkstring(L, 1);
    char file[LUA_PATH_MAX], part[LUA_PATH_MAX];
    resolve_path(L, luaL_checkstring(L, 2), file);  // now: the answers reuse lua_req.path
    if (strlen(file) + sizeof(".part") > sizeof(part))
        return luaL_error(L, "path too long: %s", file);
    memcpy(part, file, strlen(file));
    memcpy(part + strlen(file), ".part", sizeof(".part"));
    int n = http_request(L, with_options(L, 3, "GET", 1), part);
    if (n != 1)
        return n;                               // nil and why
    bool written = lua_getfield(L, -1, "size") != LUA_TNIL;     // a 2xx went to the file
    lua_pop(L, 1);
    if (written && !rename_to(part, file, true)) {
        char err[sizeof(lua_req.error)];
        memcpy(err, lua_req.error, sizeof(err));
        remove_quietly(part);
        lua_pushnil(L);
        lua_pushstring(L, err);
        return 2;
    }
    return 1;
}

static const luaL_Reg http_lib[] = {
    {"request", l_http_request}, {"get", l_http_get},   {"post", l_http_post},
    {"head", l_http_head},       {"download", l_http_download}, {NULL, NULL},
};
#endif

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
            lua_beat.beats++;
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

// --- sos.hash, sos.hmac (on core 0: lua_hash.c), sos.base64 (lua_codec.c) ---------------------

static const char *const hash_names[] = {"sha1", "sha256", "sha384", "sha512", NULL};

static bool hash_step(unsigned step, unsigned arg, const char *data, size_t len) {
    lua_req.type = LR_HASH;
    lua_req.offset = step;
    lua_req.arg = arg;
    memcpy(lua_req.data, data, len);
    lua_req.len = (uint32_t)len;
    request();
    return lua_req.result >= 0;
}

// The last step of a hash that went well so far (ok): push the digest, raw or in hex, or nil and
// why.
static int finish_hash(lua_State *L, bool ok, bool raw) {
    check_cancel(L);
    if (!ok || !hash_step(LUA_HASH_FINAL, 0, "", 0))
        return fail(L);
    size_t n = lua_req.len;
    if (raw) {
        lua_pushlstring(L, (const char *)lua_req.data, n);
    } else {
        char hex[2 * LUA_HASH_MAX];
        lua_hex(lua_req.data, n, hex);
        lua_pushlstring(L, hex, 2 * n);
    }
    return 1;
}

// Hash msg (with key, for an HMAC) on core 0, 4 KB per request; push the digest, raw or in hex.
static int run_hash(lua_State *L, unsigned arg, const char *key, size_t key_len, const char *msg,
                    size_t msg_len, bool raw) {
    if (key_len > LUA_REQ_DATA)
        return luaL_error(L, "key longer than %d bytes", LUA_REQ_DATA);
    bool ok = hash_step(LUA_HASH_BEGIN, arg, key, key_len);
    for (size_t at = 0; ok && at < msg_len; at += LUA_REQ_DATA) {
        check_cancel(L);
        size_t n = msg_len - at < LUA_REQ_DATA ? msg_len - at : LUA_REQ_DATA;
        ok = hash_step(LUA_HASH_UPDATE, arg, msg + at, n);
    }
    return finish_hash(L, ok, raw);
}

// sos.hash(alg, data [, raw]): the digest of data in hex (raw bytes if raw is true).
static int l_hash(lua_State *L) {
    unsigned alg = (unsigned)luaL_checkoption(L, 1, NULL, hash_names);
    size_t len;
    const char *msg = luaL_checklstring(L, 2, &len);
    return run_hash(L, alg, "", 0, msg, len, lua_toboolean(L, 3));
}

// sos.hmac(alg, key, data [, raw]): the HMAC of data with key, in hex (raw bytes if raw).
static int l_hmac(lua_State *L) {
    unsigned alg = (unsigned)luaL_checkoption(L, 1, NULL, hash_names);
    size_t key_len, len;
    const char *key = luaL_checklstring(L, 2, &key_len);
    const char *msg = luaL_checklstring(L, 3, &len);
    return run_hash(L, alg | LUA_HASH_HMAC, key, key_len, msg, len, lua_toboolean(L, 4));
}

// sos.hashfile(path [, alg [, raw]]): the digest of the file at path (alg default "sha256"), in
// hex or raw. Core 0 reads and hashes it (LUA_HASH_FILE), for up to 20 ms per request
// (lua_host.c), so the file's bytes never come to core 1 and a script's memory doesn't limit
// its size.
static int l_hashfile(lua_State *L) {
    char path[LUA_PATH_MAX];
    resolve_path(L, luaL_checkstring(L, 1), path);
    unsigned alg = (unsigned)luaL_checkoption(L, 2, "sha256", hash_names);
    bool raw = lua_toboolean(L, 3);
    bool ok = hash_step(LUA_HASH_BEGIN, alg, "", 0);
    memcpy(lua_req.path, path, strlen(path) + 1);   // LR_HASH answers leave it alone
    while (ok) {
        check_cancel(L);
        lua_req.type = LR_HASH;
        lua_req.offset = LUA_HASH_FILE;
        lua_req.arg = alg;
        lua_req.len = 0;
        request();
        ok = lua_req.result >= 0;
        if (lua_req.result == 0)
            break;                              // the end of the file
    }
    return finish_hash(L, ok, raw);
}

static bool url_variant(lua_State *L, int idx) {
    static const char *const variants[] = {"std", "url", NULL};
    return luaL_checkoption(L, idx, "std", variants) == 1;
}

// sos.base64.encode(s [, "url"])
static int l_b64_encode(lua_State *L) {
    size_t n;
    const uint8_t *s = (const uint8_t *)luaL_checklstring(L, 1, &n);
    bool url = url_variant(L, 2);
    luaL_Buffer b;
    char *out = luaL_buffinitsize(L, &b, lua_b64_encoded_len(n, url));
    lua_b64_encode(s, n, out, url);
    luaL_pushresultsize(&b, lua_b64_encoded_len(n, url));
    return 1;
}

// sos.base64.decode(s [, "url"]): the bytes, or nil and a message if s isn't base64.
static int l_b64_decode(lua_State *L) {
    size_t n, len;
    const char *s = luaL_checklstring(L, 1, &n);
    bool url = url_variant(L, 2);
    luaL_Buffer b;
    uint8_t *out = (uint8_t *)luaL_buffinitsize(L, &b, n / 4 * 3 + 3);
    if (!lua_b64_decode(s, n, out, &len, url)) {
        lua_pushnil(L);
        lua_pushstring(L, url ? "not URL-safe base64" : "not base64");
        return 2;
    }
    luaL_pushresultsize(&b, len);
    return 1;
}

static const luaL_Reg base64_lib[] = {
    {"encode", l_b64_encode}, {"decode", l_b64_decode}, {NULL, NULL},
};

// --- sos.json (lua_json.c) ------------------------------------------------------------------------

// sos.json.decode(s): the value, or nil and where the text stops being JSON.
static int l_json_decode(lua_State *L) {
    size_t n;
    const char *s = luaL_checklstring(L, 1, &n);
    return lua_json_decode(L, s, n);
}

// sos.json.encode(v): the text.
static int l_json_encode(lua_State *L) {
    luaL_checkany(L, 1);
    lua_json_encode(L, 1);
    return 1;
}

static const luaL_Reg json_lib[] = {
    {"decode", l_json_decode}, {"encode", l_json_encode}, {NULL, NULL},
};

// --- read-only tables: sos.sensors, sos.mem, ... (lua_health.c on core 0) ---------------------

// Ask core 0 for table `which` (with argument `arg`) and push it, or push nil and the error.
static int info_arg(lua_State *L, lua_health_table_t which, uint32_t arg) {
    lua_req.type = LR_INFO;
    lua_req.offset = (uint32_t)which;
    lua_req.arg = arg;
    request();
    check_cancel(L);
    if (lua_req.result < 0)
        return fail(L);
    if (!lua_info_push(L, lua_req.data, lua_req.len))
        return luaL_error(L, "malformed answer from core 0");
    return 1;
}

static int info(lua_State *L, lua_health_table_t which) {
    return info_arg(L, which, 0);
}

static int l_sensors(lua_State *L) { return info(L, LH_SENSORS); }
static int l_version(lua_State *L) { return info(L, LH_VERSION); }
static int l_reset_reason(lua_State *L) { return info(L, LH_RESET); }
static int l_glitch(lua_State *L) { return info(L, LH_GLITCH); }
#if SOS_HAVE_NET
static int l_wifi_status(lua_State *L) { return info(L, LH_WIFI); }
#endif
static int l_sd_info(lua_State *L) { return info(L, LH_SD); }
#if SOS_HAVE_CHAT
static int l_who(lua_State *L) { return info(L, LH_WHO); }
#endif
#if SOS_HAVE_SSH
static int l_ssh_status(lua_State *L) { return info(L, LH_SSH); }
#endif
#if SOS_HAVE_TLS
static int l_tls_roots(lua_State *L) { return info(L, LH_TLS); }
#endif

#if SOS_HAVE_CHAT
// sos.wall(msg): to every shell; returns how many (0: nobody is logged in), or nil and why.
static int l_wall(lua_State *L) {
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    if (len >= LUA_REQ_DATA)
        len = LUA_REQ_DATA - 1;
    lua_req.type = LR_WALL;
    memcpy(lua_req.data, s, len);
    lua_req.data[len] = '\0';
    request();
    if (lua_req.result < 0)
        return fail(L);
    lua_pushinteger(L, (lua_Integer)lua_req.result);
    return 1;
}
#endif

// sos.clock_sync(): start an NTS sync, as `time sync` does; true, or nil and why. The result shows
// in sos.clock() once the sync is done.
static int l_clock_sync(lua_State *L) {
    lua_req.type = LR_TIMESYNC;
    request();
    if (lua_req.result < 0)
        return fail(L);
    lua_pushboolean(L, 1);
    return 1;
}

// sos.random(n): n random bytes (from core 0's DRBG, 4 KB per request).
static int l_random(lua_State *L) {
    lua_Integer n = luaL_checkinteger(L, 1);
    luaL_argcheck(L, n >= 0 && n <= 128 * 1024, 1, "from 0 to 131072 bytes (a script's memory)");
    luaL_Buffer b;
    char *out = luaL_buffinitsize(L, &b, (size_t)n);
    for (lua_Integer at = 0; at < n; at += LUA_REQ_DATA) {
        lua_Integer k = n - at < LUA_REQ_DATA ? n - at : LUA_REQ_DATA;
        lua_req.type = LR_RANDOM;
        lua_req.offset = (uint32_t)k;
        request();
        if (lua_req.result < 0)
            return fail(L);
        memcpy(out + at, lua_req.data, (size_t)k);
    }
    luaL_pushresultsize(&b, (size_t)n);
    return 1;
}
static int l_clock(lua_State *L) { return info(L, LH_CLOCK); }
static int l_health(lua_State *L) { return info(L, LH_HEALTH); }

// Core 0's figures, and the script's own allocation, which core 1 keeps.
static int l_mem(lua_State *L) {
    if (info(L, LH_MEM) != 1)
        return 2;
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, mem_used);
    lua_setfield(L, -2, "used");
    lua_pushinteger(L, mem_peak);
    lua_setfield(L, -2, "peak");
    lua_pushinteger(L, mem_limit);
    lua_setfield(L, -2, "limit");
    lua_setfield(L, -2, "script");
    return 1;
}

// The history holds LOG_HISTORY_SIZE (4 KB) of text: a few pages at most, even with lines logged
// while it's read. A cap, so a log written faster than it's read can't keep the loop going.
#define DMESG_PAGES 8

// sos.dmesg([cursor]): the log entries after cursor (all of them without one), the cursor to pass
// next time, and whether entries after the given cursor were lost. Pages from core 0 are joined
// into one list.
static int l_dmesg(lua_State *L) {
    lua_Integer since = luaL_optinteger(L, 1, 0);
    luaL_argcheck(L, since >= 0 && since <= UINT32_MAX, 1, "not a cursor from sos.dmesg");
    uint32_t cursor = (uint32_t)since;
    bool lost = false;
    lua_newtable(L);                            // the list
    int list = lua_gettop(L);
    lua_Integer n = 0;
    for (int page = 0; page < DMESG_PAGES; page++) {
        if (info_arg(L, LH_DMESG, cursor) != 1)
            return 2;                           // nil and the error
        int t = lua_gettop(L);
        if (lua_getfield(L, t, "entries") == LUA_TTABLE) {
            lua_Integer k = (lua_Integer)lua_rawlen(L, -1);
            for (lua_Integer i = 1; i <= k; i++) {
                lua_rawgeti(L, -1, i);
                lua_rawseti(L, list, ++n);
            }
        }
        lua_getfield(L, t, "cursor");
        cursor = (uint32_t)lua_tointeger(L, -1);
        lua_getfield(L, t, "lost");
        lost = lost || lua_toboolean(L, -1);    // a later page too, if the log overtook it
        lua_getfield(L, t, "more");
        bool more = lua_toboolean(L, -1);
        lua_settop(L, list);
        if (!more)
            break;
    }
    lua_pushinteger(L, cursor);
    lua_pushboolean(L, lost);
    return 3;
}

static const luaL_Reg sd_lib[] = {
    {"info", l_sd_info}, {NULL, NULL},
};

#if SOS_HAVE_SSH
static const luaL_Reg ssh_lib[] = {
    {"status", l_ssh_status}, {NULL, NULL},
};
#endif

#if SOS_HAVE_TLS
static const luaL_Reg tls_lib[] = {
    {"roots", l_tls_roots}, {NULL, NULL},
};
#endif

#if SOS_HAVE_NET
// --- sos.wifi.scan, sos.net.resolve, sos.ping (on core 0: lua_net.c) -------------------------

// One of the slow network requests; its answer (lua_info.h records) pushed as a table.
static int net_request(lua_State *L, lua_req_type_t type, const char *host, uint32_t count) {
    if (host) {
        size_t n = strlen(host);
        if (n >= LUA_PATH_MAX)
            return luaL_error(L, "host name too long");
        memcpy(lua_req.path, host, n + 1);
    }
    lua_req.type = type;
    lua_req.offset = count;
    request();
    check_cancel(L);
    if (lua_req.result < 0)
        return fail(L);
    if (!lua_info_push(L, lua_req.data, lua_req.len))
        return luaL_error(L, "malformed answer from core 0");
    return 1;
}

// sos.wifi.scan(): the networks around, strongest first: { {ssid=, rssi=}, ... }.
static int l_wifi_scan(lua_State *L) {
    return net_request(L, LR_SCAN, NULL, 0);
}

// sos.net.resolve(host): its IPv4 address as text, or nil and why.
static int l_net_resolve(lua_State *L) {
    if (net_request(L, LR_RESOLVE, luaL_checkstring(L, 1), 0) != 1)
        return 2;
    lua_getfield(L, -1, "address");
    return 1;
}

// sos.ping(host [, count]): count requests (4 by default, at most 100), one a second; then
// {address=, sent=, received=, errors=, not_sent=, min_ms=, avg_ms=, max_ms=}, or nil and why.
static int l_ping(lua_State *L) {
    const char *host = luaL_checkstring(L, 1);
    lua_Integer count = luaL_optinteger(L, 2, 4);
    luaL_argcheck(L, count >= 1 && count <= 100, 2, "from 1 to 100");
    return net_request(L, LR_PING, host, (uint32_t)count);
}

static const luaL_Reg net_lib[] = {
    {"resolve", l_net_resolve}, {NULL, NULL},
};

static const luaL_Reg wifi_lib[] = {
    {"status", l_wifi_status}, {"scan", l_wifi_scan}, {NULL, NULL},
};
#endif

// sos.features(): what this image was built with (docs/design/build-features.md §7), as
// {NET=true, SSH=false, ...}, and its features line as `line`. No request: it's the build's.
static int l_features(lua_State *L) {
    static const struct {
        const char *name;
        bool on;
    } f[] = {
        {"NET", SOS_HAVE_NET}, {"TLS", SOS_HAVE_TLS}, {"HTTP", SOS_HAVE_HTTP},
        {"NTS", SOS_HAVE_NTS}, {"SSH", SOS_HAVE_SSH}, {"LUA", SOS_HAVE_LUA},
        {"CHAT", SOS_HAVE_CHAT}, {"PAGER", SOS_HAVE_PAGER}, {"EDITOR", SOS_HAVE_EDITOR},
        {"MAN", SOS_HAVE_MAN}, {"FSCK", SOS_HAVE_FSCK}, {"ARCHIVE", SOS_HAVE_ARCHIVE},
    };
    lua_createtable(L, 0, (int)(sizeof(f) / sizeof(f[0])) + 1);
    for (unsigned i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        lua_pushboolean(L, f[i].on);
        lua_setfield(L, -2, f[i].name);
    }
    lua_pushstring(L, SOS_FEATURE_LIST);
    lua_setfield(L, -2, "line");
    return 1;
}

static const luaL_Reg sos_lib[] = {
    {"log", l_log},       {"time", l_time},     {"uptime", l_uptime}, {"sleep", l_sleep},
    {"read", l_read},     {"write", l_write},   {"append", l_append}, {"list", l_list},
    {"remove", l_remove}, {"mkdir", l_mkdir},   {"every", l_every},   {"after", l_after},
    {"cancel", l_cancel}, {"sensors", l_sensors}, {"mem", l_mem},     {"version", l_version},
    {"reset_reason", l_reset_reason},           {"glitch", l_glitch}, {"dmesg", l_dmesg},
    {"reboot", l_reboot}, {"date", l_date},     {"clock", l_clock},   {"hash", l_hash},
    {"localdate", l_localdate},                 {"localtime", l_localtime}, {"zone", l_zone},
    {"hmac", l_hmac},     {"health", l_health},
#if SOS_HAVE_CHAT
    {"who", l_who},       {"wall", l_wall},
#endif
    {"hashfile", l_hashfile},
    {"clock_sync", l_clock_sync},               {"random", l_random}, {"rename", l_rename},
    {"copy", l_copy},     {"stat", l_stat},     {"exists", l_exists}, {"features", l_features},
#if SOS_HAVE_NET
    {"ping", l_ping},
#endif
#ifdef SOS_TEST_COMMANDS
    {"spin", l_spin},
#endif
    {NULL, NULL},
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
#if SOS_HAVE_HTTP
    luaL_newlib(L, http_lib);
    lua_setfield(L, -2, "http");
#endif
#if SOS_HAVE_NET
    luaL_newlib(L, wifi_lib);
    lua_setfield(L, -2, "wifi");
#endif
    luaL_newlib(L, sd_lib);
    lua_setfield(L, -2, "sd");
#if SOS_HAVE_SSH
    luaL_newlib(L, ssh_lib);
    lua_setfield(L, -2, "ssh");
#endif
#if SOS_HAVE_TLS
    luaL_newlib(L, tls_lib);
    lua_setfield(L, -2, "tls");
#endif
#if SOS_HAVE_NET
    luaL_newlib(L, net_lib);
    lua_setfield(L, -2, "net");
#endif
    luaL_newlib(L, base64_lib);
    lua_setfield(L, -2, "base64");
    luaL_newlib(L, json_lib);
    lua_json_push_null(L);
    lua_setfield(L, -2, "null");
    lua_setfield(L, -2, "json");
#if SOS_HAVE_HTTP
    luaL_newmetatable(L, HTTP_CLOSER);
    lua_pushcfunction(L, http_closer);
    lua_setfield(L, -2, "__close");
    lua_pop(L, 1);
#endif
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

// Tab at the prompt, under lua_pcall (run_repl): the names that could finish repl_line.
static int l_repl_complete(lua_State *L) {
    size_t used;
    bool more;
    unsigned n = lua_complete(L, lua_job.repl_line, (char *)lua_req.data, LUA_REQ_DATA, &used,
                              &more);
    lua_job.repl_names_len = (uint32_t)used;
    lua_job.repl_names = n;
    return 0;
}

static void run_repl(lua_State *L) {
    lua_pushliteral(L, "");                         // 2: unfinished statement
    for (;;) {
        lua_beat.waiting = LUA_WAIT_LINE;
        while (!lua_job.repl_cmd && !lua_job.repl_quit)
            __wfe();
        __dmb();
        lua_beat.waiting = LUA_WAIT_NONE;
        if (lua_job.repl_quit)
            break;
        if (lua_job.repl_complete) {                // Tab: names, not a line (no code runs)
            // Protected: the walk allocates (the path's strings), and a failed allocation
            // raises an error, which mustn't end the session.
            lua_job.repl_names = 0;
            lua_job.repl_names_len = 0;
            lua_pushcfunction(L, l_repl_complete);
            if (lua_pcall(L, 0, 0, 0) != LUA_OK)
                lua_job.repl_names = lua_job.repl_names_len = 0;
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
#if SOS_HAVE_HTTP
        http_open = false;
#endif
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
        lua_beat.waiting = LUA_WAIT_NONE;
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
