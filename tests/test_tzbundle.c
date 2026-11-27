// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/time/tzbundle.c: looking zones up in tz/zones.bundle, the real one and
// made-up ones that aren't well formed, and the closest names offered for one not found.

#include <stdio.h>
#include <string.h>

#include "test.h"
#include "time/tzbundle.h"

static bool read_file(void *ctx, char *buf, size_t cap) {
    return fgets(buf, (int)cap, ctx) != NULL;
}

// A bundle in memory, read as fgets() would.
typedef struct {
    const char *p;
} text_t;

static bool read_text(void *ctx, char *buf, size_t cap) {
    text_t *t = ctx;
    if (!*t->p)
        return false;
    size_t n = 0;
    while (n + 1 < cap && t->p[n]) {
        n++;
        if (t->p[n - 1] == '\n')
            break;
    }
    memcpy(buf, t->p, n);
    buf[n] = '\0';
    t->p += n;
    return true;
}

static tzbundle_result_t find_in(const char *text, const char *name, tzbundle_find_t *out) {
    text_t t = {text};
    return tzbundle_find(read_text, &t, name, out);
}

static tzbundle_result_t find_real(const char *name, tzbundle_find_t *out) {
    FILE *f = fopen(TZ_BUNDLE, "r");
    CHECK(f != NULL);
    if (!f)
        return TZBUNDLE_NOT_A_BUNDLE;
    tzbundle_result_t r = tzbundle_find(read_file, f, name, out);
    fclose(f);
    return r;
}

TEST(version_from_header) {
    char v[TZBUNDLE_VERSION_MAX + 1];
    CHECK(tzbundle_version("# tzdata 2026f: each zone's rule\n", v));
    CHECK(!strcmp(v, "2026f"));
    CHECK(!tzbundle_version("# tzdata 2026f each zone\n", v));
    CHECK(!tzbundle_version("# tzdata : x\n", v));
    CHECK(!tzbundle_version("# tzdata 2026F: x\n", v));
    CHECK(!tzbundle_version("# tzdata 2026fffffffffffffffff: x\n", v));
    CHECK(!tzbundle_version("Europe/Madrid CET-1CEST,M3.5.0,M10.5.0/3\n", v));
    CHECK(!tzbundle_version("", v));
}

TEST(split_lines) {
    char line[128], *n, *r;
    strcpy(line, "Europe/Madrid CET-1CEST,M3.5.0,M10.5.0/3\n");
    CHECK(tzbundle_split(line, &n, &r));
    CHECK(!strcmp(n, "Europe/Madrid") && !strcmp(r, "CET-1CEST,M3.5.0,M10.5.0/3"));
    strcpy(line, "Etc/GMT+5 <-05>5\r\n");
    CHECK(tzbundle_split(line, &n, &r));
    CHECK(!strcmp(n, "Etc/GMT+5") && !strcmp(r, "<-05>5"));
    const char *bad[] = {
        "", "\n", "Europe/Madrid\n", " CET-1\n", "Europe/Madrid \n", "Europe/Madrid CET-1 x\n",
        "Europe/Ma\x1b[2Jdrid CET-1\n", "Europe/Madrid CET-1\x07\n", "# tzdata 2026f: x\n",
        "A234567890123456789012345678901234567890 UTC0\n",                    // 40: too long
        "UTC A234567890123456789012345678901234567890123456789012345678901234\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        strcpy(line, bad[i]);
        CHECK(!tzbundle_split(line, &n, &r));
    }
    strcpy(line, "A23456789012345678901234567890123456789 UTC0");    // 39, no newline
    CHECK(tzbundle_split(line, &n, &r));
}

TEST(find_in_the_real_bundle) {
    tzbundle_find_t f;
    CHECK_EQ(find_real("Europe/Madrid", &f), TZBUNDLE_FOUND);
    CHECK(!strcmp(f.name, "Europe/Madrid"));
    CHECK(!strcmp(f.rule, "CET-1CEST,M3.5.0,M10.5.0/3"));
    CHECK(f.version[0] != '\0');
    CHECK_EQ(find_real("europe/MADRID", &f), TZBUNDLE_FOUND);       // case aside
    CHECK(!strcmp(f.name, "Europe/Madrid"));
    CHECK_EQ(find_real("america/new york", &f), TZBUNDLE_FOUND);    // ' ' as '_'
    CHECK(!strcmp(f.name, "America/New_York"));
    CHECK_EQ(find_real("Australia/Lord_Howe", &f), TZBUNDLE_FOUND);
    CHECK(!strcmp(f.rule, "<+1030>-10:30<+11>-11,M10.1.0,M4.1.0"));
    CHECK_EQ(find_real("UTC", &f), TZBUNDLE_FOUND);
    CHECK_EQ(find_real("Africa/Abidjan", &f), TZBUNDLE_FOUND);      // the bundle's first line
    CHECK_EQ(find_real("Zulu", &f), TZBUNDLE_FOUND);                // the bundle's last line

    CHECK_EQ(find_real("Europe/Madird", &f), TZBUNDLE_NOT_FOUND);
    CHECK(f.near_count >= 1 && !strcmp(f.near[0], "Europe/Madrid"));
    CHECK(f.zones > 500);
    CHECK_EQ(f.skipped, 0);
    CHECK_EQ(find_real("madrid", &f), TZBUNDLE_NOT_FOUND);         // not a zone's whole name
    CHECK(f.near_count >= 1 && !strcmp(f.near[0], "Europe/Madrid"));
    CHECK_EQ(find_real("new york", &f), TZBUNDLE_NOT_FOUND);
    CHECK(f.near_count >= 1 && !strcmp(f.near[0], "America/New_York"));
    CHECK_EQ(find_real("Europe", &f), TZBUNDLE_NOT_FOUND);         // inside many: the first few
    CHECK_EQ(f.near_count, TZBUNDLE_NEAR);
    CHECK_EQ(find_real("qqqqqqqqqqqq", &f), TZBUNDLE_NOT_FOUND);
    CHECK_EQ(f.near_count, 0);
    CHECK_EQ(find_real("", &f), TZBUNDLE_NOT_FOUND);
    CHECK_EQ(f.near_count, 0);
}

// What's on the card may be anything: no header, long lines, lines that aren't zones, rules
// that don't parse, no newline at the end.
TEST(find_in_bundles_not_well_formed) {
    tzbundle_find_t f;
    CHECK_EQ(find_in("", "UTC", &f), TZBUNDLE_NOT_A_BUNDLE);
    CHECK_EQ(find_in("UTC UTC0\n", "UTC", &f), TZBUNDLE_NOT_A_BUNDLE);
    CHECK_EQ(find_in("<html>\n", "UTC", &f), TZBUNDLE_NOT_A_BUNDLE);
    CHECK_EQ(find_in("# tzdata 2026f: x\nUTC UTC0", "UTC", &f), TZBUNDLE_FOUND);    // no newline
    CHECK(!strcmp(f.version, "2026f"));
    CHECK_EQ(find_in("# tzdata 2026f: x\nBad/Rule CET-1CEST\nUTC UTC0\n", "Bad/Rule", &f),
             TZBUNDLE_NOT_FOUND);                                   // its rule doesn't parse
    CHECK_EQ(f.zones, 1);
    CHECK_EQ(f.skipped, 1);
    // A long line, the end of which looks like a zone, is skipped whole.
    char text[512];
    snprintf(text, sizeof(text), "# tzdata 2026f: x\n%0*d Fake/Zone UTC0\nUTC UTC0\n", 200, 0);
    CHECK_EQ(find_in(text, "Fake/Zone", &f), TZBUNDLE_NOT_FOUND);
    CHECK_EQ(find_in(text, "UTC", &f), TZBUNDLE_FOUND);
    CHECK_EQ(f.skipped, 1);
    // So is a long header's rest.
    snprintf(text, sizeof(text), "# tzdata 2026f: %0*d\nFake/Zone UTC0\nUTC UTC0\n", 200, 0);
    CHECK_EQ(find_in(text, "UTC", &f), TZBUNDLE_FOUND);
}

TEST(distance) {
    CHECK_EQ(tzbundle_distance("Europe/Madrid", "Europe/Madrid"), 0);
    CHECK_EQ(tzbundle_distance("madrid", "Europe/Madrid"), 0);
    CHECK_EQ(tzbundle_distance("Europe/Madird", "Europe/Madrid"), 2);
    CHECK_EQ(tzbundle_distance("york", "America/New_York"), 1);
    CHECK_EQ(tzbundle_distance("", "UTC"), 99);
    CHECK(tzbundle_distance("qqqq", "UTC") >= 3);
    CHECK(tzbundle_has_prefix("Europe/Madrid", "europe/"));
    CHECK(tzbundle_has_prefix("Europe/Madrid", ""));
    CHECK(!tzbundle_has_prefix("Europe/Madrid", "Europe/Madrid/x"));
    CHECK(!tzbundle_has_prefix("UTC", "Europe"));
}

TEST_MAIN(
    RUN(version_from_header);
    RUN(split_lines);
    RUN(find_in_the_real_bundle);
    RUN(find_in_bundles_not_well_formed);
    RUN(distance);
)
