// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Text from outside (an HTTP response, say) made safe to print on a terminal. Pure C, host
// tested.
//
// Printable ASCII, newline and tab pass. So does valid UTF-8 for U+00A0 and above, so accented
// and non-Latin text shows as itself. Everything that a terminal could act on becomes '.':
// C0 controls (ESC starts escape sequences), DEL, and the C1 controls, both as raw bytes
// 0x80-0x9F (0x9B is CSI on 8-bit terminals) and UTF-8-encoded (U+0080-U+009F). Invalid UTF-8
// becomes '.' too. CR is dropped, since the shell turns '\n' into CRLF itself.
//
// Input can come in pieces: a UTF-8 sequence split between calls is held until it's complete.

#ifndef SOS_TERMTEXT_H
#define SOS_TERMTEXT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t pend[4];        // an incomplete UTF-8 sequence carried over
    uint8_t n;              // bytes in pend
    uint8_t need;           // its total length
} termtext_t;

#define TERMTEXT_INIT {{0}, 0, 0}

// Filter len bytes into out, which must hold len + 3 bytes. Returns the number written.
size_t termtext_filter(termtext_t *t, const uint8_t *in, size_t len, char *out);

// End of the input: an incomplete sequence left over becomes '.'. out must hold 3 bytes.
// Returns the number written.
size_t termtext_flush(termtext_t *t, char *out);

#endif
