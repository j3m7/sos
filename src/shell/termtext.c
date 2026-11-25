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

// The code point of a complete, valid UTF-8 sequence.
static uint32_t decode(const uint8_t *p, unsigned n) {
    uint32_t cp = p[0] & (0x7F >> n);
    for (unsigned k = 1; k < n; k++)
        cp = cp << 6 | (p[k] & 0x3F);
    return cp;
}

// Code points that print nothing, or change how the text around them is shown, so the text
// on screen can differ from its bytes: C1 controls, bidi controls, the known channels for
// hiding text (zero-width characters, tag characters, the variation selectors supplement) and
// a few other invisible format characters. Not every invisible character: that would take the
// Unicode tables. ZWNJ and ZWJ (U+200C, U+200D) pass, since scripts such as Persian and the
// Indic ones, and emoji sequences, need them; so do U+FE00-U+FE0F (emoji and CJK variants).
// Tags are the exception on purpose: blocking them breaks the subdivision flags (England,
// Scotland, Wales), but they can carry hidden ASCII.
// Keep in step with is_hidden() in tests/test_termtext.c, which the fuzz test checks against.
static bool hidden(uint32_t cp) {
    return (cp >= 0x80 && cp <= 0x9F)           // C1 controls
        || cp == 0x061C                         // ARABIC LETTER MARK
        || cp == 0x180E                         // MONGOLIAN VOWEL SEPARATOR
        || cp == 0x200B                         // ZERO WIDTH SPACE
        || cp == 0x200E || cp == 0x200F         // LRM, RLM
        || (cp >= 0x2028 && cp <= 0x202E)       // LS, PS; LRE, RLE, PDF, LRO, RLO
        || (cp >= 0x2060 && cp <= 0x206F)       // WORD JOINER, invisible operators, LRI, RLI,
                                                // FSI, PDI, deprecated format controls
        || cp == 0xFEFF                         // ZERO WIDTH NO-BREAK SPACE (BOM)
        || (cp >= 0xFFF9 && cp <= 0xFFFB)       // interlinear annotation controls
        || (cp >= 0xE0000 && cp <= 0xE007F)     // tag characters
        || (cp >= 0xE0100 && cp <= 0xE01EF);    // variation selectors supplement
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
        // Complete.
        if (hidden(decode(t->pend, t->n))) {
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
