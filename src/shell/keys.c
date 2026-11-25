// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Terminal key decoding (see keys.h). Depends only on the C library, so it builds on the host
// for unit tests.

#include "shell/keys.h"

enum { ST_NONE, ST_ESC, ST_CSI, ST_SS3 };

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
    case 'R':
        return k->nparams == 2 ? KEY_CURSOR_POS : KEYS_NONE;
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
        } else {
            k->state = c == 'O' ? ST_SS3 : ST_NONE;     // anything else: Alt+key, dropped
        }
        return KEYS_NONE;
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
        } else if (c >= 0x3c && c <= 0x3f) {   // '<' '=' '>' '?'
            k->priv = true;
        } else if (c >= 0x40 && c <= 0x7e) {   // final byte
            k->state = ST_NONE;
            return csi_key(k, c);
        }
        return KEYS_NONE;                       // ':' and intermediates (0x20-0x2f): skipped
    default:                                    // SS3: one byte
        k->state = ST_NONE;
        return ss3_key(c);
    }
}
