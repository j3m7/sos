// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The record format of lua_info.h: core 0 writes, core 1 reads into a Lua table.

#include "lua/lua_info.h"

#include <string.h>

#include "lua.h"

// --- writing -------------------------------------------------------------------------------------

void lua_info_begin(lua_info_t *w, uint8_t *buf, size_t cap) {
    *w = (lua_info_t){.buf = buf, .cap = cap};
}

static void put(lua_info_t *w, const void *p, size_t n) {
    if (w->failed || n > w->cap - w->len) {
        w->failed = true;
        return;
    }
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}

static void head(lua_info_t *w, char type, const char *name) {
    size_t n = strlen(name);
    if (n > UINT8_MAX) {
        w->failed = true;
        return;
    }
    uint8_t h[2] = {(uint8_t)type, (uint8_t)n};
    put(w, h, sizeof(h));
    put(w, name, n);
}

void lua_info_int(lua_info_t *w, const char *name, int64_t v) {
    head(w, 'i', name);
    put(w, &v, sizeof(v));
}

void lua_info_num(lua_info_t *w, const char *name, double v) {
    head(w, 'n', name);
    put(w, &v, sizeof(v));
}

void lua_info_bool(lua_info_t *w, const char *name, bool v) {
    uint8_t b = v ? 1 : 0;
    head(w, 'b', name);
    put(w, &b, 1);
}

void lua_info_str(lua_info_t *w, const char *name, const char *s) {
    if (!s)
        return;
    size_t n = strlen(s);
    if (n > UINT16_MAX) {
        w->failed = true;
        return;
    }
    uint16_t len = (uint16_t)n;
    head(w, 's', name);
    put(w, &len, sizeof(len));
    put(w, s, n);
}

void lua_info_open(lua_info_t *w, const char *name) {
    if (++w->depth > LUA_INFO_DEPTH)
        w->failed = true;
    head(w, '{', name);
}

void lua_info_close(lua_info_t *w) {
    if (--w->depth < 0)
        w->failed = true;
    uint8_t t = '}';
    put(w, &t, 1);
}

size_t lua_info_end(lua_info_t *w) {
    return w->failed || w->depth != 0 ? 0 : w->len;
}

// --- reading -------------------------------------------------------------------------------------

typedef struct {
    const uint8_t *p, *end;
} reader_t;

static bool take(reader_t *r, void *out, size_t n) {
    if (n > (size_t)(r->end - r->p))
        return false;
    memcpy(out, r->p, n);
    r->p += n;
    return true;
}

// The records, each pushed as key and value and stored with lua_rawset into the table below
// them. A table's key and table stay on the stack until its '}'.
static bool read_all(lua_State *L, reader_t *r) {
    int depth = 0;
    while (r->p < r->end) {
        uint8_t type = *r->p++;
        if (type == '}') {
            if (depth-- == 0)
                return false;
            lua_rawset(L, -3);
            continue;
        }
        uint8_t n;
        if (!take(r, &n, 1) || n > (size_t)(r->end - r->p))
            return false;
        lua_pushlstring(L, (const char *)r->p, n);
        r->p += n;
        switch (type) {
        case 'i': {
            int64_t v;
            if (!take(r, &v, sizeof(v)))
                return false;
            lua_pushinteger(L, (lua_Integer)v);
            break;
        }
        case 'n': {
            double v;
            if (!take(r, &v, sizeof(v)))
                return false;
            lua_pushnumber(L, (lua_Number)v);
            break;
        }
        case 'b': {
            uint8_t v;
            if (!take(r, &v, 1))
                return false;
            lua_pushboolean(L, v != 0);
            break;
        }
        case 's': {
            uint16_t len;
            if (!take(r, &len, sizeof(len)) || len > (size_t)(r->end - r->p))
                return false;
            lua_pushlstring(L, (const char *)r->p, len);
            r->p += len;
            break;
        }
        case '{':
            if (++depth > LUA_INFO_DEPTH)
                return false;
            lua_newtable(L);
            continue;                           // filled until its '}'
        default:
            return false;
        }
        lua_rawset(L, -3);
    }
    return depth == 0;
}

bool lua_info_push(lua_State *L, const uint8_t *buf, size_t len) {
    // Each open table holds two slots; a record pushes two more.
    if (!lua_checkstack(L, 2 * LUA_INFO_DEPTH + 4))
        return false;
    int top = lua_gettop(L);
    lua_newtable(L);
    reader_t r = {buf, buf + len};
    if (!read_all(L, &r)) {
        lua_settop(L, top);
        return false;
    }
    return true;
}
