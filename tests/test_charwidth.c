// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/shell/charwidth.c: terminal column widths.

#include "shell/charwidth.h"
#include "test.h"

TEST(narrow) {
    CHECK_EQ(charwidth('a'), 1);
    CHECK_EQ(charwidth(0xE9), 1);           // é
    CHECK_EQ(charwidth(0x3B1), 1);          // Greek alpha
    CHECK_EQ(charwidth(0x416), 1);          // Cyrillic Zhe
    CHECK_EQ(charwidth(0x20AC), 1);         // euro sign
    CHECK_EQ(charwidth(0x2500), 1);         // box drawing
    CHECK_EQ(charwidth(0xFF61), 1);         // halfwidth katakana punctuation
    CHECK_EQ(charwidth(0x10FFFF), 1);
}

TEST(wide) {
    CHECK_EQ(charwidth(0x4E00), 2);         // first CJK unified ideograph
    CHECK_EQ(charwidth(0x6F22), 2);         // 漢
    CHECK_EQ(charwidth(0x9FFF), 2);
    CHECK_EQ(charwidth(0x3042), 2);         // hiragana a
    CHECK_EQ(charwidth(0xAC00), 2);         // Hangul syllable ga
    CHECK_EQ(charwidth(0xFF21), 2);         // fullwidth A
    CHECK_EQ(charwidth(0x1F600), 2);        // grinning face
    CHECK_EQ(charwidth(0x1F44D), 2);        // thumbs up
    CHECK_EQ(charwidth(0x20000), 2);        // CJK extension B
    CHECK_EQ(charwidth(0x2FFFD), 2);        // reserved plane 2: CJK
    CHECK_EQ(charwidth(0x3000), 2);         // ideographic space
}

TEST(zero_width) {
    CHECK_EQ(charwidth(0x300), 0);          // combining grave, first in the table
    CHECK_EQ(charwidth(0x301), 0);          // combining acute
    CHECK_EQ(charwidth(0x36F), 0);
    CHECK_EQ(charwidth(0x200C), 0);         // ZWNJ
    CHECK_EQ(charwidth(0x200D), 0);         // ZWJ
    CHECK_EQ(charwidth(0xFE0F), 0);         // emoji variation selector
    CHECK_EQ(charwidth(0x1160), 0);         // Hangul medial vowel
    CHECK_EQ(charwidth(0x093F), 1);         // Devanagari vowel sign I: Mc, takes a column
    CHECK_EQ(charwidth(0x0941), 0);         // Devanagari vowel sign U: Mn
}

TEST(range_edges) {
    CHECK_EQ(charwidth(0x2FF), 1);          // just below the first table entry
    CHECK_EQ(charwidth(0x303F), 1);         // ideographic half fill space: narrow
    CHECK_EQ(charwidth(0x2E7F), 1);         // just before the CJK radicals
    CHECK_EQ(charwidth(0x2E80), 2);         // first CJK radical
    CHECK_EQ(charwidth(0xA000), 2);         // Yi syllables are wide
    CHECK_EQ(charwidth(0xD7A3), 2);         // last Hangul syllable
    CHECK_EQ(charwidth(0xD7A4), 1);
}

TEST_MAIN(
    RUN(narrow);
    RUN(wide);
    RUN(zero_width);
    RUN(range_edges);
)
