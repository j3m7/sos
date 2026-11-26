// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/sealedonly.c: whether a board refuses unsealed records, decided from
// the three OTP rows of its marker. The rule under test is that it fails closed: only blank
// rows say "no".

#include "core/sealedonly.h"
#include "test.h"

#define P SEALEDONLY_PATTERN
#define UNREADABLE 0xffffffffu

static sealedonly_t decide(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t rows[SEALEDONLY_COPIES] = { a, b, c };
    return sealedonly_decide(rows);
}

TEST(blank_is_an_open_board) {
    CHECK_EQ(decide(0, 0, 0), SEALEDONLY_NO);
}

TEST(the_marker) {
    CHECK_EQ(decide(P, P, P), SEALEDONLY_YES);
}

// One copy wrong, either way: the vote still reads the marker.
TEST(one_bad_copy_still_reads_as_the_marker) {
    CHECK_EQ(decide(P, P, 0), SEALEDONLY_YES);
    CHECK_EQ(decide(0, P, P), SEALEDONLY_YES);
    CHECK_EQ(decide(P, 0xffffff, P), SEALEDONLY_YES);
    CHECK_EQ(decide(P, P, P | 0x000012), SEALEDONLY_YES);
}

// Anything that isn't blank and isn't the marker is still "yes": a write cut short after one
// row, bits set on top of the marker, or bits set on a blank board.
TEST(anything_else_is_yes_too) {
    CHECK_EQ(decide(P, 0, 0), SEALEDONLY_YES_DAMAGED);          // only the first row written
    CHECK_EQ(decide(0, 0, P), SEALEDONLY_YES_DAMAGED);
    CHECK_EQ(decide(1, 0, 0), SEALEDONLY_YES_DAMAGED);          // one stray bit
    CHECK_EQ(decide(0, 0x800000, 0), SEALEDONLY_YES_DAMAGED);
    CHECK_EQ(decide(0xffffff, 0xffffff, 0xffffff), SEALEDONLY_YES_DAMAGED); // every bit set
    CHECK_EQ(decide(P | 2, P | 2, P), SEALEDONLY_YES_DAMAGED);  // two copies with a bit added
    CHECK_EQ(decide(P & ~0x20u, P & ~0x20u, P & ~0x20u), SEALEDONLY_YES_DAMAGED);  // a bit short
}

// A row that can't be read comes back as all 32 bits set. It must never read as "no".
TEST(unreadable_is_yes) {
    CHECK_EQ(decide(UNREADABLE, UNREADABLE, UNREADABLE), SEALEDONLY_YES_UNREADABLE);
    CHECK_EQ(decide(UNREADABLE, 0, 0), SEALEDONLY_YES_UNREADABLE);
    CHECK_EQ(decide(0, 0, UNREADABLE), SEALEDONLY_YES_UNREADABLE);
    CHECK_EQ(decide(P, UNREADABLE, P), SEALEDONLY_YES_UNREADABLE);
}

// OTP bits can be set but never cleared. Starting from the marker, no set of added bits may
// turn the answer into "no": try each single bit on each row, and each pair of rows filled.
TEST(the_marker_cannot_be_removed_by_setting_bits) {
    for (unsigned row = 0; row < SEALEDONLY_COPIES; row++) {
        for (unsigned bit = 0; bit < 24; bit++) {
            uint32_t rows[SEALEDONLY_COPIES] = { P, P, P };
            rows[row] |= 1u << bit;
            CHECK(sealedonly_decide(rows) != SEALEDONLY_NO);
        }
        uint32_t rows[SEALEDONLY_COPIES] = { P, P, P };
        rows[row] = 0xffffff;
        rows[(row + 1) % SEALEDONLY_COPIES] = 0xffffff;
        CHECK(sealedonly_decide(rows) != SEALEDONLY_NO);
    }
}

// The top eight bits of a readable row are zero on the chip; a value with some of them set
// (not all 32) is still judged on its 24 bits and never as "no" unless those are blank.
TEST(only_the_24_row_bits_count) {
    CHECK_EQ(decide(0x01000000, 0, 0), SEALEDONLY_NO);
    CHECK_EQ(decide(0x01000000 | P, P, P), SEALEDONLY_YES);
}

TEST(constants) {
    CHECK_EQ(SEALEDONLY_ROW, 0x0c0);            // page 3, row 0: the first user page
    CHECK_EQ(SEALEDONLY_ROW / 64, 3);
    CHECK(SEALEDONLY_PATTERN != 0 && SEALEDONLY_PATTERN < 0x1000000);
}

TEST_MAIN(
    RUN(blank_is_an_open_board);
    RUN(the_marker);
    RUN(one_bad_copy_still_reads_as_the_marker);
    RUN(anything_else_is_yes_too);
    RUN(unreadable_is_yes);
    RUN(the_marker_cannot_be_removed_by_setting_bits);
    RUN(only_the_24_row_bits_count);
    RUN(constants);
)
