// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// A small record format for the read-only tables a script asks core 0 for (sos.sensors, sos.mem,
// ...; the LR_INFO request in lua_bridge.h). Core 0 writes named values and nested tables into
// the request's data with the lua_info_* writer; core 1 turns them into one Lua table with
// lua_info_push(). So a new table needs code on core 0 only.
//
// Records, back to back, in the byte order of the chip (both cores are the same):
//   type (1 byte), then for every type but '}': name length (1 byte) and the name, then
//   'i' int64 | 'n' double | 'b' 1 byte (0 or 1) | 's' uint16 length and the bytes |
//   '{' nothing (a table starts) | '}' no name, nothing (the table ends).
// Tables nest at most LUA_INFO_DEPTH deep.

#ifndef SOS_LUA_INFO_H
#define SOS_LUA_INFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LUA_INFO_DEPTH 4

// --- writing (core 0) ----------------------------------------------------------------------------

typedef struct {
    uint8_t *buf;
    size_t cap, len;
    int depth;
    bool failed;                // out of room, a name too long, or tables unbalanced
} lua_info_t;

void lua_info_begin(lua_info_t *w, uint8_t *buf, size_t cap);
void lua_info_int(lua_info_t *w, const char *name, int64_t v);
void lua_info_num(lua_info_t *w, const char *name, double v);
void lua_info_bool(lua_info_t *w, const char *name, bool v);
void lua_info_str(lua_info_t *w, const char *name, const char *s);  // NULL: nothing written
void lua_info_open(lua_info_t *w, const char *name);
void lua_info_close(lua_info_t *w);

// Bytes written, or 0 if anything failed (then the buffer holds nothing usable).
size_t lua_info_end(lua_info_t *w);

// --- reading (core 1) ----------------------------------------------------------------------------

struct lua_State;

// Push the records in buf[0..len) as one table. Returns false, with nothing pushed, if they
// aren't well formed (cut short, an unknown type, unbalanced or too deep tables).
bool lua_info_push(struct lua_State *L, const uint8_t *buf, size_t len);

#endif
