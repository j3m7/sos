// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/chipinfo.c: naming the RP2350 revision for `version`.

#include <string.h>

#include "core/chipinfo.h"
#include "test.h"

TEST(known_revisions) {
    CHECK(!strcmp(chipinfo_revision_name(2, 2), "A2"));
    CHECK(!strcmp(chipinfo_revision_name(3, 3), "A3"));
    CHECK(!strcmp(chipinfo_revision_name(3, 4), "A4"));
}

// A pair the SDK doesn't document gets no name: guessing "A4" for a chip that might be an A2
// would hide the revision the security work has to know.
TEST(unknown_pairs_have_no_name) {
    CHECK(chipinfo_revision_name(2, 4) == NULL);
    CHECK(chipinfo_revision_name(3, 2) == NULL);
    CHECK(chipinfo_revision_name(3, 5) == NULL);
    CHECK(chipinfo_revision_name(4, 4) == NULL);
    CHECK(chipinfo_revision_name(0, 0) == NULL);
}

TEST(format) {
    char s[64];
    chipinfo_format(s, sizeof(s), 3, 4);
    CHECK(!strcmp(s, "RP2350 A4 (revision 3, ROM 4)"));
    chipinfo_format(s, sizeof(s), 2, 2);
    CHECK(!strcmp(s, "RP2350 A2 (revision 2, ROM 2)"));
    chipinfo_format(s, sizeof(s), 5, 7);
    CHECK(!strcmp(s, "RP2350, unknown revision (revision 5, ROM 7)"));
}

TEST(format_cuts_to_the_buffer) {
    char s[12];
    memset(s, 'x', sizeof(s));
    chipinfo_format(s, 8, 3, 4);
    CHECK(!strcmp(s, "RP2350 "));
    CHECK_EQ(s[8], 'x');
}

TEST_MAIN(
    RUN(known_revisions);
    RUN(unknown_pairs_have_no_name);
    RUN(format);
    RUN(format_cuts_to_the_buffer);
)
