// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// sos.json: see lua_json.h.

#include "lua/lua_json.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"

static char null_marker;                        // its address is sos.json.null

void lua_json_push_null(lua_State *L) {
    lua_pushlightuserdata(L, &null_marker);
}

static bool is_null(lua_State *L, int idx) {
    return lua_type(L, idx) == LUA_TLIGHTUSERDATA && lua_touserdata(L, idx) == &null_marker;
}

// --- decoding ------------------------------------------------------------------------------------

typedef struct {
    lua_State *L;
    const char *s, *p, *end;
    const char *err;                            // set on the first error, with p where it was
} dec_t;

static bool fail(dec_t *d, const char *msg) {
    if (!d->err)
        d->err = msg;
    return false;
}

static void skip_ws(dec_t *d) {
    while (d->p < d->end && (*d->p == ' ' || *d->p == '\t' || *d->p == '\n' || *d->p == '\r'))
        d->p++;
}

static bool literal(dec_t *d, const char *word) {
    size_t n = strlen(word);
    if ((size_t)(d->end - d->p) < n || memcmp(d->p, word, n))
        return fail(d, "invalid value");
    d->p += n;
    return true;
}

static int hex4(const char *p) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= c - '0';
        else if (c >= 'a' && c <= 'f')
            v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            v |= c - 'A' + 10;
        else
            return -1;
    }
    return v;
}

static void add_utf8(luaL_Buffer *b, unsigned cp) {
    char u[4];
    size_t n;
    if (cp < 0x80) {
        u[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        u[0] = (char)(0xc0 | cp >> 6);
        u[1] = (char)(0x80 | (cp & 0x3f));
        n = 2;
    } else if (cp < 0x10000) {
        u[0] = (char)(0xe0 | cp >> 12);
        u[1] = (char)(0x80 | (cp >> 6 & 0x3f));
        u[2] = (char)(0x80 | (cp & 0x3f));
        n = 3;
    } else {
        u[0] = (char)(0xf0 | cp >> 18);
        u[1] = (char)(0x80 | (cp >> 12 & 0x3f));
        u[2] = (char)(0x80 | (cp >> 6 & 0x3f));
        u[3] = (char)(0x80 | (cp & 0x3f));
        n = 4;
    }
    luaL_addlstring(b, u, n);
}

// A string, *p at its opening quote; pushed. Bytes other than escapes and control characters
// are taken as they are (UTF-8 isn't checked).
static bool string(dec_t *d) {
    d->p++;
    luaL_Buffer b;
    luaL_buffinit(d->L, &b);
    for (;;) {
        const char *run = d->p;
        while (d->p < d->end && *d->p != '"' && *d->p != '\\' && (unsigned char)*d->p >= 0x20)
            d->p++;
        luaL_addlstring(&b, run, (size_t)(d->p - run));
        if (d->p >= d->end) {
            luaL_pushresult(&b);
            return fail(d, "unfinished string");
        }
        char c = *d->p;
        if (c == '"')
            break;
        if (c != '\\') {
            luaL_pushresult(&b);
            return fail(d, "control character in string");
        }
        if (d->end - d->p < 2) {
            luaL_pushresult(&b);
            return fail(d, "unfinished string");
        }
        char e = d->p[1];
        d->p += 2;
        const char *simple = strchr("\"\\/bfnrt", e);
        if (e && simple) {
            static const char out[] = "\"\\/\b\f\n\r\t";
            luaL_addchar(&b, out[simple - "\"\\/bfnrt"]);
            continue;
        }
        int cp = e == 'u' && d->end - d->p >= 4 ? hex4(d->p) : -1;
        if (cp < 0) {
            d->p -= 2;
            luaL_pushresult(&b);
            return fail(d, "invalid escape in string");
        }
        d->p += 4;
        if (cp >= 0xd800 && cp <= 0xdbff) {     // a surrogate pair: the low half must follow
            int lo = d->end - d->p >= 6 && d->p[0] == '\\' && d->p[1] == 'u' ? hex4(d->p + 2) : -1;
            if (lo < 0xdc00 || lo > 0xdfff) {
                luaL_pushresult(&b);
                return fail(d, "lone surrogate in string");
            }
            d->p += 6;
            cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
        } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            luaL_pushresult(&b);
            return fail(d, "lone surrogate in string");
        }
        add_utf8(&b, (unsigned)cp);
    }
    d->p++;                                     // the closing quote
    luaL_pushresult(&b);
    return true;
}

// A number, strictly as RFC 8259 writes it; pushed.
static bool number(dec_t *d) {
    const char *start = d->p, *p = d->p;
    bool is_int = true;
    if (p < d->end && *p == '-')
        p++;
    if (p >= d->end || *p < '0' || *p > '9')
        return fail(d, "invalid value");
    if (*p == '0')
        p++;
    else
        while (p < d->end && *p >= '0' && *p <= '9')
            p++;
    if (p < d->end && *p == '.') {
        is_int = false;
        p++;
        if (p >= d->end || *p < '0' || *p > '9')
            return fail(d, "invalid number");
        while (p < d->end && *p >= '0' && *p <= '9')
            p++;
    }
    if (p < d->end && (*p == 'e' || *p == 'E')) {
        is_int = false;
        p++;
        if (p < d->end && (*p == '+' || *p == '-'))
            p++;
        if (p >= d->end || *p < '0' || *p > '9')
            return fail(d, "invalid number");
        while (p < d->end && *p >= '0' && *p <= '9')
            p++;
    }
    size_t n = (size_t)(p - start);
    if (is_int) {                               // exact, when it fits in an integer
        bool neg = *start == '-';
        uint64_t v = 0, limit = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
        bool fits = true;
        for (const char *q = start + neg; q < p && fits; q++) {
            unsigned digit = (unsigned)(*q - '0');
            if (v > (limit - digit) / 10)
                fits = false;
            else
                v = v * 10 + digit;
        }
        if (fits) {
            lua_pushinteger(d->L, neg ? (lua_Integer)(0 - v) : (lua_Integer)v);
            d->p = p;
            return true;
        }
    }
    char tmp[64];                               // otherwise a float, as Lua reads one
    if (n >= sizeof(tmp))
        return fail(d, "number too long");
    memcpy(tmp, start, n);
    tmp[n] = '\0';
    if (!lua_stringtonumber(d->L, tmp))
        return fail(d, "invalid number");
    if (!isfinite(lua_tonumber(d->L, -1))) {    // 1e999: JSON can't say infinity, nor can we
        lua_pop(d->L, 1);
        return fail(d, "number out of range");
    }
    d->p = p;
    return true;
}

static bool value(dec_t *d, int depth);

static bool container(dec_t *d, int depth, bool object) {
    if (depth >= LUA_JSON_DEPTH)
        return fail(d, "nested too deep");
    luaL_checkstack(d->L, 4, "JSON");
    d->p++;
    lua_newtable(d->L);
    skip_ws(d);
    if (d->p < d->end && *d->p == (object ? '}' : ']')) {
        d->p++;
        return true;
    }
    for (lua_Integer i = 1;; i++) {
        skip_ws(d);
        if (object) {
            if (d->p >= d->end || *d->p != '"')
                return fail(d, "expected a string key");
            if (!string(d))
                return false;
            skip_ws(d);
            if (d->p >= d->end || *d->p != ':')
                return fail(d, "expected ':'");
            d->p++;
        }
        if (!value(d, depth + 1))
            return false;
        if (object)
            lua_rawset(d->L, -3);
        else
            lua_rawseti(d->L, -2, i);
        skip_ws(d);
        if (d->p < d->end && *d->p == ',') {
            d->p++;
            continue;
        }
        if (d->p < d->end && *d->p == (object ? '}' : ']')) {
            d->p++;
            return true;
        }
        return fail(d, object ? "expected ',' or '}'" : "expected ',' or ']'");
    }
}

static bool value(dec_t *d, int depth) {
    skip_ws(d);
    if (d->p >= d->end)
        return fail(d, "unexpected end");
    switch (*d->p) {
    case '{': return container(d, depth, true);
    case '[': return container(d, depth, false);
    case '"': return string(d);
    case 't':
    case 'f': {
        bool t = *d->p == 't';
        if (!literal(d, t ? "true" : "false"))
            return false;
        lua_pushboolean(d->L, t);
        return true;
    }
    case 'n':
        if (!literal(d, "null"))
            return false;
        lua_json_push_null(d->L);
        return true;
    default:
        return number(d);
    }
}

int lua_json_decode(lua_State *L, const char *s, size_t n) {
    int top = lua_gettop(L);
    dec_t d = {L, s, s, s + n, NULL};
    if (value(&d, 0)) {
        skip_ws(&d);
        if (d.p == d.end)
            return 1;
        fail(&d, "trailing characters");
    }
    lua_settop(L, top);
    lua_pushnil(L);
    lua_pushfstring(L, "%s at byte %d", d.err, (int)(d.p - s) + 1);
    return 2;
}

// --- encoding ------------------------------------------------------------------------------------
//
// The text grows in a userdata kept at a fixed stack slot (replaced by a bigger one as it grows),
// so the collector frees it however the encoding ends: core 1 has no __gc.

typedef struct {
    lua_State *L;
    int slot;                                   // the stack index of the buffer's userdata
    char *p;
    size_t len, cap;
} enc_t;

static void put(enc_t *e, const char *s, size_t n) {
    if (n > e->cap - e->len) {
        size_t cap = e->cap * 2;
        while (cap - e->len < n)
            cap *= 2;
        char *q = lua_newuserdatauv(e->L, cap, 0);
        memcpy(q, e->p, e->len);
        lua_replace(e->L, e->slot);
        e->p = q;
        e->cap = cap;
    }
    memcpy(e->p + e->len, s, n);
    e->len += n;
}

static void put_s(enc_t *e, const char *s) {
    put(e, s, strlen(s));
}

static void put_string(enc_t *e, const char *s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    put(e, "\"", 1);
    const char *run = s;
    for (const char *p = s; p < s + n; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 0x20 && c != '"' && c != '\\')
            continue;
        put(e, run, (size_t)(p - run));
        run = p + 1;
        switch (c) {
        case '"':  put_s(e, "\\\""); break;
        case '\\': put_s(e, "\\\\"); break;
        case '\n': put_s(e, "\\n"); break;
        case '\r': put_s(e, "\\r"); break;
        case '\t': put_s(e, "\\t"); break;
        case '\b': put_s(e, "\\b"); break;
        case '\f': put_s(e, "\\f"); break;
        default: {
            char u[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
            put(e, u, sizeof(u));
            break;
        }
        }
    }
    put(e, run, (size_t)(s + n - run));
    put(e, "\"", 1);
}

// An object's key, for sorting. A string key points at the key in the table being encoded,
// which keeps it alive: encoding runs no Lua code, so the table can't change meanwhile. An integer
// key points at its decimal text, kept alive by object()'s anchor table, and is looked up again
// by reading that text back. 12 bytes on the board.
typedef struct {
    const char *s;
    size_t n;
    bool is_int;
} json_key_t;

static int key_cmp(const void *a, const void *b) {
    const json_key_t *x = a, *y = b;
    size_t n = x->n < y->n ? x->n : y->n;
    int c = memcmp(x->s, y->s, n);
    return c ? c : (x->n > y->n) - (x->n < y->n);
}

static void encode(enc_t *e, int idx, int depth);

// The table at idx (absolute) as an array, if its keys are exactly 1..n with n > 0; false
// otherwise (nothing written).
static bool try_array(enc_t *e, int idx, int depth) {
    lua_State *L = e->L;
    lua_Unsigned n = lua_rawlen(L, idx), count = 0;
    if (n == 0)
        return false;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        lua_pop(L, 1);
        if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 1 ||
            (lua_Unsigned)lua_tointeger(L, -1) > n || ++count > n) {
            lua_pop(L, 1);
            return false;
        }
    }
    if (count != n)
        return false;
    put(e, "[", 1);
    for (lua_Unsigned i = 1; i <= n; i++) {
        if (i > 1)
            put(e, ",", 1);
        lua_rawgeti(L, idx, (lua_Integer)i);
        encode(e, lua_gettop(L), depth + 1);
        lua_pop(L, 1);
    }
    put(e, "]", 1);
    return true;
}

static void object(enc_t *e, int idx, int depth) {
    lua_State *L = e->L;
    size_t count = 0, ints = 0;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        lua_pop(L, 1);
        count++;
        if (lua_isinteger(L, -1))
            ints++;
    }
    // The keys' order in a userdata, and the integer keys' text in a table that keeps it alive
    // (only when there are integer keys).
    json_key_t *k = lua_newuserdatauv(L, (count ? count : 1) * sizeof(json_key_t), 0);
    int helpers = 1, anchors = 0;
    if (ints) {
        lua_createtable(L, (int)(ints < 1000000 ? ints : 0), 0);
        anchors = lua_gettop(L);
        helpers = 2;
    }
    size_t n = 0, a = 0;
    lua_pushnil(L);
    while (lua_next(L, idx)) {                  // the same `count` keys as above
        lua_pop(L, 1);
        json_key_t *kk = &k[n++];
        kk->is_int = false;
        if (lua_type(L, -1) == LUA_TSTRING) {
            kk->s = lua_tolstring(L, -1, &kk->n);   // a string already: not converted in place
        } else if (lua_isinteger(L, -1)) {
            kk->is_int = true;
            lua_pushfstring(L, "%I", (LUAI_UACINT)lua_tointeger(L, -1));
            kk->s = lua_tolstring(L, -1, &kk->n);
            lua_rawseti(L, anchors, (lua_Integer)++a);
        } else {
            luaL_error(L, "can't encode a %s key in JSON", luaL_typename(L, -1));
        }
    }
    qsort(k, n, sizeof(json_key_t), key_cmp);
    for (size_t i = 1; i < n; i++)              // the string "1" and the integer 1, say
        if (!key_cmp(&k[i - 1], &k[i]))
            luaL_error(L, "two keys are both \"%s\" in JSON", k[i].s);
    put(e, "{", 1);
    for (size_t i = 0; i < n; i++) {
        if (i)
            put(e, ",", 1);
        put_string(e, k[i].s, k[i].n);
        put(e, ":", 1);
        if (k[i].is_int)
            lua_stringtonumber(L, k[i].s);      // the integer again, from the text it gave
        else
            lua_pushlstring(L, k[i].s, k[i].n);
        lua_rawget(L, idx);
        encode(e, lua_gettop(L), depth + 1);
        lua_pop(L, 1);
    }
    put(e, "}", 1);
    lua_pop(L, helpers);                        // the order, and the anchors if any
}

static void encode(enc_t *e, int idx, int depth) {
    lua_State *L = e->L;
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        put_s(e, "null");
        break;
    case LUA_TBOOLEAN:
        put_s(e, lua_toboolean(L, idx) ? "true" : "false");
        break;
    case LUA_TNUMBER: {
        if (!lua_isinteger(L, idx) && !isfinite(lua_tonumber(L, idx)))
            luaL_error(L, "can't encode %s in JSON", lua_tonumber(L, idx) != lua_tonumber(L, idx)
                       ? "NaN" : "an infinity");
        size_t n;
        lua_pushvalue(L, idx);                  // a copy: tostring changes it
        const char *s = lua_tolstring(L, -1, &n);
        put(e, s, n);
        lua_pop(L, 1);
        break;
    }
    case LUA_TSTRING: {
        size_t n;
        const char *s = lua_tolstring(L, idx, &n);
        put_string(e, s, n);
        break;
    }
    case LUA_TTABLE:
        if (depth >= LUA_JSON_DEPTH)
            luaL_error(L, "can't encode tables nested deeper than %d in JSON (or one that "
                       "contains itself)", LUA_JSON_DEPTH);
        luaL_checkstack(L, 8, "JSON");
        if (!try_array(e, idx, depth))
            object(e, idx, depth);
        break;
    case LUA_TLIGHTUSERDATA:
        if (is_null(L, idx)) {
            put_s(e, "null");
            break;
        }
        /* fall through */
    default:
        luaL_error(L, "can't encode a %s in JSON", luaL_typename(L, idx));
    }
}

void lua_json_encode(lua_State *L, int idx) {
    idx = lua_absindex(L, idx);
    enc_t e = {L, 0, NULL, 0, 64};
    e.p = lua_newuserdatauv(L, e.cap, 0);
    e.slot = lua_gettop(L);
    encode(&e, idx, 0);
    lua_pushlstring(L, e.p, e.len);
    lua_replace(L, e.slot);                     // the text where the buffer was
}
