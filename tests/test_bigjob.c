// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/files/bigjob.c: one archive job at a time across all shells.

#include "files/bigjob.h"
#include "test.h"

TEST(one_at_a_time) {
    CHECK(bigjob_running() == NULL);
    CHECK(bigjob_try_begin("fetch") == NULL);
    CHECK(!strcmp(bigjob_running(), "fetch"));
    const char *other = bigjob_try_begin("gunzip");
    CHECK(other && !strcmp(other, "fetch"));        // refused, naming the one running
    CHECK(!strcmp(bigjob_running(), "fetch"));      // still the first
    bigjob_end();
    CHECK(bigjob_running() == NULL);
    CHECK(bigjob_try_begin("gunzip") == NULL);      // free again
    bigjob_end();
    bigjob_end();                                   // harmless with none running
    CHECK(bigjob_running() == NULL);
}

TEST(busy_message) {
    char msg[80];
    snprintf(msg, sizeof(msg), BIGJOB_BUSY_FMT, "tar");
    CHECK(!strcmp(msg, "busy: tar is running in another shell\n"));
}

TEST_MAIN(
    RUN(one_at_a_time);
    RUN(busy_message);
)
