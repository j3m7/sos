// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "shell/termtext.h"

#include <stdbool.h>

// The length of the UTF-8 sequence that lead byte c starts, or 0 if c can't start one.
// C0/C1 are excluded: 0xC2 can start one, but the C1 check happens once it's complete.
static unsigned seq_len(uint8_t c) {
    if (c >= 0xC2 && c <= 0xDF)
        return 2;
    if (c >= 0xE0 && c <= 0xEF)
        return 3;
    if (c >= 0xF0 && c <= 0xF4)
        return 4;
    return 0;
}

// Is c allowed as the second byte after lead? (Rules out overlong forms, surrogates and code
// points above U+10FFFF, per RFC 3629.) Later bytes just need to be continuation bytes.
static int second_ok(uint8_t lead, uint8_t c) {
    switch (lead) {
    case 0xE0: return c >= 0xA0 && c <= 0xBF;
    case 0xED: return c >= 0x80 && c <= 0x9F;
    case 0xF0: return c >= 0x90 && c <= 0xBF;
    case 0xF4: return c >= 0x80 && c <= 0x8F;
    default:   return c >= 0x80 && c <= 0xBF;
    }
}

// One byte that isn't part of a pending sequence.
static size_t plain(termtext_t *t, uint8_t c, char *out) {
    if (c == '\r')
        return 0;
    if (c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7F)) {
        *out = (char)c;
        return 1;
    }
    unsigned len = seq_len(c);
    if (len) {
        t->pend[0] = c;
        t->n = 1;
        t->need = (uint8_t)len;
        return 0;
    }
    *out = '.';                                 // control, DEL, C1 byte, stray continuation
    return 1;
}

size_t termtext_filter(termtext_t *t, const uint8_t *in, size_t len, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = in[i];
        if (!t->n) {
            o += plain(t, c, out + o);
            continue;
        }
        bool ok = t->n == 1 ? second_ok(t->pend[0], c) : (c >= 0x80 && c <= 0xBF);
        if (!ok) {
            // The sequence is broken: show it as one '.', and take c afresh.
            out[o++] = '.';
            t->n = 0;
            o += plain(t, c, out + o);
            continue;
        }
        t->pend[t->n++] = c;
        if (t->n < t->need)
            continue;
        // Complete. U+0080-U+009F (C2 80 - C2 9F) are the C1 controls.
        if (t->need == 2 && t->pend[0] == 0xC2 && t->pend[1] <= 0x9F) {
            out[o++] = '.';
        } else {
            for (unsigned k = 0; k < t->n; k++)
                out[o++] = (char)t->pend[k];
        }
        t->n = 0;
    }
    return o;
}

size_t termtext_flush(termtext_t *t, char *out) {
    if (!t->n)
        return 0;
    t->n = 0;
    out[0] = '.';
    return 1;
}
