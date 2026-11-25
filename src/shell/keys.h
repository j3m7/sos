// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_KEYS_H
#define SOS_KEYS_H

#include <stdbool.h>

// Terminal key decoding. Terminals send special keys as escape sequences: CSI "ESC [ params
// final" (arrows, Home/End, Delete, PgUp/PgDn, the cursor position report), SS3 "ESC O x"
// (application-mode arrows, F1-F4) and "ESC x" (Alt+key). keys_feed() takes one input byte at a
// time, so a sequence may be split across reads, and turns each complete sequence into one key
// code. Pure C, host tested.

typedef struct {
    unsigned char state;        // where in a sequence we are (keys.c)
    unsigned char nparams;      // CSI parameters seen so far (separated by ';')
    bool priv;                  // CSI with a private marker ('<', '=', '>', '?'): never a key
    unsigned short param[2];    // the first two CSI parameters, capped at 9999
} keys_t;

#define KEYS_INIT { 0 }

// keys_feed() results: a plain byte (0x00-0xff) is returned as itself; a special key as one of
// these. KEYS_NONE: the byte was part of a sequence, nothing to act on (yet).
enum {
    KEYS_NONE = -1,
    KEY_UP = 0x100,
    KEY_DOWN,
    KEY_RIGHT,
    KEY_LEFT,
    KEY_HOME,
    KEY_END,
    KEY_INSERT,
    KEY_DELETE,
    KEY_PGUP,
    KEY_PGDN,
    // Answer to "ESC [ 6 n" (where is the cursor?): "ESC [ row ; col R". keys_t.param[0] is the
    // row and param[1] the column, both from 1. The same bytes are Ctrl/Shift+F3 on some
    // terminals ("ESC [ 1 ; 5 R"), so treat it as an answer only after asking.
    KEY_CURSOR_POS,
};

// Decode one byte. A control character (below 0x20) in the middle of a sequence abandons the
// sequence and is returned as itself, so a stray ESC can't swallow Enter or Ctrl-C. Unknown
// sequences, modifier variants included, give KEYS_NONE (Ctrl+Right is just Right).
int keys_feed(keys_t *k, unsigned char c);

#endif
