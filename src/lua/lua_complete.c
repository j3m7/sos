// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Tab at the Lua prompt (see lua_complete.h).

#include "lua/lua_complete.h"

#include <string.h>

static bool is_name_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_name_char(char c) {
    return is_name_start(c) || (c >= '0' && c <= '9');
}

static bool is_reserved(const char *s, size_t n) {
    static const char *const words[] = {
        "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if",
        "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while",
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
        if (strlen(words[i]) == n && !memcmp(words[i], s, n))
            return true;
    return false;
}

// Is s[0..n) a name that can follow a '.'?
static bool is_name(const char *s, size_t n) {
    if (n == 0 || !is_name_start(s[0]) || is_reserved(s, n))
        return false;
    for (size_t i = 1; i < n; i++)
        if (!is_name_char(s[i]))
            return false;
    return true;
}

// The table to look names up in for the value on top of the stack: itself if it's a table, else
// its metatable's __index if that is a table (raw access). Replaces the value with that table and
// returns true, or pops it and returns false.
static bool lookup_table(lua_State *L) {
    if (lua_type(L, -1) == LUA_TTABLE)
        return true;
    if (lua_getmetatable(L, -1)) {
        lua_pushliteral(L, "__index");
        lua_rawget(L, -2);
        lua_remove(L, -2);                      // the metatable
        if (lua_type(L, -1) == LUA_TTABLE) {
            lua_remove(L, -2);                  // the value
            return true;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return false;
}

typedef struct {
    const char *head;           // the token up to and including its last separator
    size_t head_len;
    const char *prefix;         // the name being typed
    size_t prefix_len;
    char *out;
    size_t cap, used;
    unsigned count;
    bool more;
} names_t;

// Offer the string keys of the table on top of the stack that start with the prefix, except
// those `skip` (a table at that stack index, or 0) has too.
static void add_keys(lua_State *L, names_t *nm, int skip) {
    int t = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, t)) {
        lua_pop(L, 1);                          // the value: only the key matters
        if (lua_type(L, -1) != LUA_TSTRING)
            continue;
        size_t n;
        const char *k = lua_tolstring(L, -1, &n);
        if (n < nm->prefix_len || memcmp(k, nm->prefix, nm->prefix_len) || !is_name(k, n))
            continue;
        if (skip) {
            lua_pushvalue(L, -1);
            lua_rawget(L, skip);
            bool there = !lua_isnil(L, -1);
            lua_pop(L, 1);
            if (there)
                continue;
        }
        size_t need = nm->head_len + n + 1;
        if (nm->used + need > nm->cap) {
            nm->more = true;
            continue;
        }
        memcpy(nm->out + nm->used, nm->head, nm->head_len);
        memcpy(nm->out + nm->used + nm->head_len, k, n);
        nm->out[nm->used + need - 1] = '\0';
        nm->used += need;
        nm->count++;
    }
}

unsigned lua_complete(lua_State *L, const char *token, char *out, size_t cap, size_t *used,
                      bool *more) {
    *used = 0;
    *more = false;
    size_t len = strlen(token);
    size_t sep = len;                           // the last '.' or ':', or len if none
    while (sep > 0 && token[sep - 1] != '.' && token[sep - 1] != ':')
        sep--;
    names_t nm = {token, sep, token + sep, len - sep, out, cap, 0, 0, false};
    for (size_t i = 0; i < nm.prefix_len; i++)
        if (!is_name_char(nm.prefix[i]))
            return 0;
    // With a separator, a name must come before it, and right before it: not ".x" or "a..x".
    if (sep > 0 && (sep == 1 || token[sep - 2] == '.' || token[sep - 2] == ':'))
        return 0;
    if (!lua_checkstack(L, 8))
        return 0;
    int top = lua_gettop(L);

    // Walk the path before the last separator: names joined by '.' (a ':' only at the end).
    lua_pushglobaltable(L);
    size_t i = 0;
    while (sep > 0 && i < sep - 1) {
        size_t j = i;
        while (j < sep - 1 && token[j] != '.' && token[j] != ':')
            j++;
        if ((j < sep - 1 && token[j] == ':') || !is_name(token + i, j - i) || !lookup_table(L)) {
            lua_settop(L, top);                 // a ':' inside the path, or not a name
            return 0;
        }
        lua_pushlstring(L, token + i, j - i);
        lua_rawget(L, -2);
        lua_remove(L, -2);
        i = j + 1;
    }
    if (!lookup_table(L)) {
        lua_settop(L, top);
        return 0;
    }
    add_keys(L, &nm, 0);
    // Then what the table's metatable's __index table adds (an object's methods).
    int own = lua_gettop(L);
    if (lua_getmetatable(L, own)) {
        lua_pushliteral(L, "__index");
        lua_rawget(L, -2);
        if (lua_type(L, -1) == LUA_TTABLE)
            add_keys(L, &nm, own);
    }
    lua_settop(L, top);
    *used = nm.used;
    *more = nm.more;
    return nm.count;
}
