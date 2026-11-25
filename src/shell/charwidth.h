// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_CHARWIDTH_H
#define SOS_CHARWIDTH_H

#include <stdint.h>

// How many terminal columns a printable code point takes: 2 for East Asian wide and fullwidth
// characters (CJK, Hangul syllables, most emoji), 0 for combining marks, ZWNJ/ZWJ and Hangul
// medial and final jamo (they join the character before them), 1 for everything else. The
// tables (charwidth_table.h) are generated from the Unicode database by tools/gen_charwidth.py.
// Control characters aren't the caller's to print, so they aren't special here. Pure C, host
// tested.
int charwidth(uint32_t cp);

#endif
