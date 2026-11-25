// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Terminal key decoding (see keys.h). Depends only on the C library, so it builds on the host
// for unit tests.

#include "shell/keys.h"

enum { ST_NONE, ST_ESC, ST_CSI, ST_SS3, ST_ALT_UTF8 };

#define PARAM_MAX 9999

// The key a CSI "params final" means, or KEYS_NONE.
static int csi_key(const keys_t *k, unsigned char final) {
    if (k->priv)
        return KEYS_NONE;
    switch (final) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case 'R':                       // row and column count from 1
        return k->nparams == 2 && k->param[0] && k->param[1] ? KEY_CURSOR_POS : KEYS_NONE;
    case '~':                       // "ESC [ n ~" (vt220), possibly with a modifier "; m"
        switch (k->param[0]) {
        case 1: case 7: return KEY_HOME;
        case 2:         return KEY_INSERT;
        case 3:         return KEY_DELETE;
        case 4: case 8: return KEY_END;
        case 5:         return KEY_PGUP;
        case 6:         return KEY_PGDN;
        }
        return KEYS_NONE;
    }
    return KEYS_NONE;
}

static int ss3_key(unsigned char c) {
    switch (c) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    }
    return KEYS_NONE;
}

int keys_feed(keys_t *k, unsigned char c) {
    if (c == 0x1b) {
        k->state = ST_ESC;
        return KEYS_NONE;
    }
    if (k->state == ST_NONE)
        return c;
    if (c < 0x20) {                             // control character: abandon the sequence
        k->state = ST_NONE;
        return c;
    }
    switch (k->state) {
    case ST_ESC:
        if (c == '[') {
            k->state = ST_CSI;
            k->nparams = 0;
            k->priv = false;
            k->param[0] = k->param[1] = 0;
        } else if (c == 'O') {
            k->state = ST_SS3;
        } else if (c >= 0xc2 && c <= 0xf4) {    // Alt + a UTF-8 character: drop all of it
            k->state = ST_ALT_UTF8;
            k->skip = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
        } else {
            k->state = ST_NONE;                 // anything else: Alt+key, dropped
        }
        return KEYS_NONE;
    case ST_ALT_UTF8:
        if (c >= 0x80 && c <= 0xbf) {
            if (--k->skip == 0)
                k->state = ST_NONE;
            return KEYS_NONE;
        }
        k->state = ST_NONE;                     // not a continuation byte: an ordinary byte
        return c;
    case ST_CSI:
        if (c >= '0' && c <= '9') {             // parameter digits
            if (k->nparams == 0)
                k->nparams = 1;
            if (k->nparams <= 2) {
                unsigned short *p = &k->param[k->nparams - 1];
                unsigned v = *p * 10u + (unsigned)(c - '0');
                *p = v > PARAM_MAX ? PARAM_MAX : (unsigned short)v;
            }
        } else if (c == ';') {
            if (k->nparams == 0)
                k->nparams = 1;                 // ";5" is an empty first parameter
            if (k->nparams < 255)
                k->nparams++;
        } else if ((c >= 0x3c && c <= 0x3f) || (c >= 0x20 && c <= 0x2f)) {
            k->priv = true;                     // private marker or intermediate byte
        } else if (c >= 0x40 && c <= 0x7e) {   // final byte
            k->state = ST_NONE;
            return csi_key(k, c);
        }
        return KEYS_NONE;                       // ':' (sub-parameters) and DEL: skipped
    default:                                    // SS3: one byte
        k->state = ST_NONE;
        return ss3_key(c);
    }
}
