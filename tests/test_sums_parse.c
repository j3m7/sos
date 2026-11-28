// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/files/sums_parse.c: names as sha256sum prints them, a checksum list split
// into lines (fed in every block size), each line's forms (GNU text and binary, the '\' escape,
// BSD), lines refused, and the names a list may give.

#include <stdlib.h>

#include "files/sums_parse.h"
#include "test.h"

#define H64 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"   // SHA-256 of ""

static sums_line_t entry;

// Parse a copy of `text` as a SHA-256 line.
static sums_line_kind_t parse(const char *text) {
    static char line[SUMS_LINE_MAX + 1];
    snprintf(line, sizeof(line), "%s", text);
    return sums_parse_line(line, strlen(line), "SHA256", 32, &entry);
}

static const uint8_t empty_digest[32] = {
    0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
    0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55,
};

// --- names as printed --------------------------------------------------------------------------

TEST(plain_names_are_shown_as_they_are) {
    CHECK_EQ(sums_name_flags("pkg-1.2.tar.gz"), 0);
    CHECK_EQ(sums_name_flags("dir/a file"), 0);
    CHECK_EQ(sums_name_len("dir/a file"), 10);
    CHECK(sums_name_char('a') == NULL);
    CHECK(sums_name_char(' ') == NULL);
    CHECK(sums_name_char((char)0xc3) == NULL);      // UTF-8 passes
}

TEST(newline_and_backslash_are_escaped) {
    CHECK_EQ(sums_name_flags("a\nb"), SUMS_NAME_ESCAPED);
    CHECK_EQ(sums_name_flags("a\\b"), SUMS_NAME_ESCAPED);
    CHECK(!strcmp(sums_name_char('\n'), "\\n"));
    CHECK(!strcmp(sums_name_char('\\'), "\\\\"));
    CHECK_EQ(sums_name_len("a\nb\\c"), 7);
}

TEST(other_control_characters_are_flagged_and_shown_as_marks) {
    CHECK_EQ(sums_name_flags("a\x1b[2Jb"), SUMS_NAME_CONTROL);
    CHECK_EQ(sums_name_flags("a\tb"), SUMS_NAME_CONTROL);
    CHECK_EQ(sums_name_flags("a\x7f"), SUMS_NAME_CONTROL);
    CHECK_EQ(sums_name_flags("\r\n"), SUMS_NAME_CONTROL | SUMS_NAME_ESCAPED);
    CHECK(!strcmp(sums_name_char('\x1b'), "?"));
    CHECK(!strcmp(sums_name_char('\r'), "?"));
    CHECK_EQ(sums_name_len("\x1b[m"), 3);
}

// --- lines ---------------------------------------------------------------------------------------

#define MAX_LINES 16

typedef struct {
    int n;
    char text[MAX_LINES][SUMS_LINE_MAX + 1];
    unsigned number[MAX_LINES];
    bool longline[MAX_LINES];
} got_t;

// Split `data` fed `block` bytes at a time (as read from the card), as sums.c does.
static void split(const char *data, size_t size, size_t block, got_t *got) {
    static sums_lines_t l;
    memset(&l, 0, sizeof(l));
    memset(got, 0, sizeof(*got));
    size_t at = 0;
    bool eof = false;
    for (int guard = 0; guard < 100000; guard++) {
        char *line;
        size_t len;
        sums_next_t r = sums_lines_next(&l, eof, &line, &len);
        if (r == SUMS_NEXT_END)
            return;
        if (r == SUMS_NEXT_MORE) {
            size_t room;
            char *to = sums_lines_room(&l, &room);
            CHECK(room >= 1);
            size_t n = size - at < block ? size - at : block;
            n = n < room ? n : room;
            memcpy(to, data + at, n);
            at += n;
            l.len += n;
            eof = n == 0;
            continue;
        }
        if (got->n == MAX_LINES)
            return;
        got->number[got->n] = l.line;
        got->longline[got->n] = r == SUMS_NEXT_LONG;
        if (r == SUMS_NEXT_LINE) {
            CHECK(len <= SUMS_LINE_MAX);
            CHECK_EQ(line[len], '\0');
            memcpy(got->text[got->n], line, len + 1);
        }
        got->n++;
    }
    CHECK(!"no end");
}

TEST(lines_split_at_newlines_in_any_block_size) {
    const char data[] = "one\ntwo\r\n\nlast";
    static const size_t blocks[] = {1, 2, 3, 5, 512, 4096};
    for (size_t b = 0; b < sizeof(blocks) / sizeof(blocks[0]); b++) {
        got_t got;
        split(data, strlen(data), blocks[b], &got);
        CHECK_EQ(got.n, 4);
        CHECK(!strcmp(got.text[0], "one"));
        CHECK(!strcmp(got.text[1], "two\r"));       // the parser drops the '\r'
        CHECK(!strcmp(got.text[2], ""));
        CHECK(!strcmp(got.text[3], "last"));        // no '\n' at the end
        CHECK_EQ(got.number[3], 4);
    }
}

TEST(an_empty_list_has_no_lines) {
    got_t got;
    split("", 0, 512, &got);
    CHECK_EQ(got.n, 0);
}

TEST(a_line_of_the_limit_is_kept_and_a_longer_one_skipped) {
    static char data[3 * SUMS_LINE_MAX + 16];
    static const size_t blocks[] = {1, 7, 512, 4096, 5000};
    for (size_t b = 0; b < sizeof(blocks) / sizeof(blocks[0]); b++) {
        size_t n = 0;
        memset(data + n, 'a', SUMS_LINE_MAX);       // exactly the limit
        n += SUMS_LINE_MAX;
        data[n++] = '\n';
        memset(data + n, 'b', SUMS_LINE_MAX + 1);   // one over
        n += SUMS_LINE_MAX + 1;
        data[n++] = '\n';
        memcpy(data + n, "after\n", 6);
        n += 6;
        memset(data + n, 'c', SUMS_LINE_MAX + 1);   // one over, the last, without '\n'
        n += SUMS_LINE_MAX + 1;
        got_t got;
        split(data, n, blocks[b], &got);
        CHECK_EQ(got.n, 4);
        CHECK(!got.longline[0] && strlen(got.text[0]) == SUMS_LINE_MAX);
        CHECK(got.longline[1]);
        CHECK_EQ(got.number[1], 2);
        CHECK(!got.longline[2] && !strcmp(got.text[2], "after"));
        CHECK(got.longline[3]);
        CHECK_EQ(got.number[3], 4);
    }
}

TEST(a_last_line_of_the_limit_without_newline_is_kept) {
    static char data[SUMS_LINE_MAX];
    memset(data, 'x', sizeof(data));
    got_t got;
    split(data, sizeof(data), 4096, &got);
    CHECK_EQ(got.n, 1);
    CHECK(!got.longline[0] && strlen(got.text[0]) == SUMS_LINE_MAX);
    split(data, sizeof(data), 1, &got);
    CHECK_EQ(got.n, 1);
    CHECK(!got.longline[0]);
}

// --- one line ------------------------------------------------------------------------------------

TEST(gnu_text_and_binary_forms) {
    CHECK_EQ(parse(H64 "  pkg-1.2.tar.gz"), SUMS_LINE_OK);
    CHECK(!strcmp(entry.name, "pkg-1.2.tar.gz"));
    CHECK_MEM(entry.digest, empty_digest, 32);
    CHECK_EQ(parse(H64 " *pkg.bin"), SUMS_LINE_OK);
    CHECK(!strcmp(entry.name, "pkg.bin"));
    CHECK_EQ(parse(H64 "  name with  spaces "), SUMS_LINE_OK);
    CHECK(!strcmp(entry.name, "name with  spaces "));
    CHECK_EQ(parse(H64 "  *star"), SUMS_LINE_OK);   // text mode: the '*' is the name's
    CHECK(!strcmp(entry.name, "*star"));
    CHECK_EQ(parse(H64 "  dos\r"), SUMS_LINE_OK);   // a list written with CRLF
    CHECK(!strcmp(entry.name, "dos"));
}

TEST(hex_in_either_case) {
    char upper[] = H64 "  f";
    for (char *p = upper; *p != ' '; p++)
        if (*p >= 'a' && *p <= 'f')
            *p = (char)(*p - 'a' + 'A');
    CHECK_EQ(parse(upper), SUMS_LINE_OK);
    CHECK_MEM(entry.digest, empty_digest, 32);
}

TEST(hex_of_the_wrong_length_or_not_hex_is_bad) {
    CHECK_EQ(parse("e3b0  f"), SUMS_LINE_BAD);
    CHECK_EQ(parse(H64 "00  f"), SUMS_LINE_BAD);        // a longer hash (SHA-512's list)
    CHECK_EQ(parse("g" H64 "  f"), SUMS_LINE_BAD);
    char bad[] = H64 "  f";
    bad[10] = 'x';
    CHECK_EQ(parse(bad), SUMS_LINE_BAD);
    // SHA-1's length, for sha1sum, and not for sha256sum.
    char sha1[] = "da39a3ee5e6b4b0d3255bfef95601890afd80709  f";
    CHECK_EQ(sums_parse_line(sha1, strlen(sha1), "SHA1", 20, &entry), SUMS_LINE_OK);
    CHECK_EQ(parse("da39a3ee5e6b4b0d3255bfef95601890afd80709  f"), SUMS_LINE_BAD);
}

TEST(separators_and_names_that_are_bad) {
    CHECK_EQ(parse(H64 " f"), SUMS_LINE_BAD);           // one space
    CHECK_EQ(parse(H64 "\t f"), SUMS_LINE_BAD);
    CHECK_EQ(parse(H64 "  "), SUMS_LINE_BAD);           // no name
    CHECK_EQ(parse(H64 " *"), SUMS_LINE_BAD);
    CHECK_EQ(parse(H64), SUMS_LINE_BAD);
    CHECK_EQ(parse("just text"), SUMS_LINE_BAD);
    char nul[] = H64 "  a\0b";
    CHECK_EQ(sums_parse_line(nul, sizeof(nul) - 1, "SHA256", 32, &entry), SUMS_LINE_BAD);
}

TEST(blank_and_comment_lines_are_skipped) {
    CHECK_EQ(parse(""), SUMS_LINE_SKIP);
    CHECK_EQ(parse("   \t"), SUMS_LINE_SKIP);
    CHECK_EQ(parse("\r"), SUMS_LINE_SKIP);
    CHECK_EQ(parse("# SHA256 checksums"), SUMS_LINE_SKIP);
    CHECK_EQ(parse("#" H64 "  f"), SUMS_LINE_SKIP);
    CHECK_EQ(parse(" # not a comment"), SUMS_LINE_BAD);
}

TEST(escaped_names_are_unescaped) {
    CHECK_EQ(parse("\\" H64 "  a\\nb"), SUMS_LINE_OK);
    CHECK(!strcmp(entry.name, "a\nb"));
    CHECK_EQ(parse("\\" H64 "  back\\\\slash"), SUMS_LINE_OK);
    CHECK(!strcmp(entry.name, "back\\slash"));
    CHECK_EQ(parse("\\" H64 " *x\\n"), SUMS_LINE_OK);
    CHECK(!strcmp(entry.name, "x\n"));
    CHECK_EQ(parse("\\" H64 "  a\\tb"), SUMS_LINE_BAD); // only \\ and \n
    CHECK_EQ(parse("\\" H64 "  end\\"), SUMS_LINE_BAD);
    // Without the mark, a '\' is part of the name.
    CHECK_EQ(parse(H64 "  dir\\file"), SUMS_LINE_OK);
    CHECK(!strcmp(entry.name, "dir\\file"));
}

TEST(bsd_form_with_the_matching_algorithm) {
    CHECK_EQ(parse("SHA256 (pkg-1.2.tar.gz) = " H64), SUMS_LINE_OK);
    CHECK(!strcmp(entry.name, "pkg-1.2.tar.gz"));
    CHECK_MEM(entry.digest, empty_digest, 32);
    CHECK_EQ(parse("SHA256 (odd) = name) = " H64), SUMS_LINE_OK);  // the last ") = " ends it
    CHECK(!strcmp(entry.name, "odd) = name"));
    CHECK_EQ(parse("SHA256 (a b) = " H64 "\r"), SUMS_LINE_OK);
    CHECK(!strcmp(entry.name, "a b"));
    CHECK_EQ(parse("\\SHA256 (a\\nb) = " H64), SUMS_LINE_OK);
    CHECK(!strcmp(entry.name, "a\nb"));
}

TEST(bsd_form_that_is_bad) {
    CHECK_EQ(parse("SHA1 (f) = " H64), SUMS_LINE_BAD);         // another algorithm
    CHECK_EQ(parse("SHA512 (f) = " H64), SUMS_LINE_BAD);
    CHECK_EQ(parse("MD5 (f) = d41d8cd98f00b204e9800998ecf8427e"), SUMS_LINE_BAD);
    CHECK_EQ(parse("SHA256 () = " H64), SUMS_LINE_BAD);         // no name
    CHECK_EQ(parse("SHA256 (f) = " H64 "0"), SUMS_LINE_BAD);
    CHECK_EQ(parse("SHA256 (f)= " H64), SUMS_LINE_BAD);
    CHECK_EQ(parse("SHA256(f) = " H64), SUMS_LINE_BAD);
    CHECK_EQ(parse("sha256 (f) = " H64), SUMS_LINE_BAD);
    CHECK_EQ(parse("SHA256 (f) = "), SUMS_LINE_BAD);
}

TEST(a_line_of_the_limit_parses) {
    static char line[SUMS_LINE_MAX + 1];
    memcpy(line, H64 "  ", 66);
    memset(line + 66, 'n', SUMS_LINE_MAX - 66);
    CHECK_EQ(sums_parse_line(line, SUMS_LINE_MAX, "SHA256", 32, &entry), SUMS_LINE_OK);
    CHECK_EQ(strlen(entry.name), SUMS_LINE_MAX - 66);
}

// --- names in a list -----------------------------------------------------------------------------

TEST(relative_names_are_safe) {
    CHECK(sums_name_safe("pkg.tar.gz"));
    CHECK(sums_name_safe("sub/dir/f"));
    CHECK(sums_name_safe("./f"));
    CHECK(sums_name_safe("a..b"));
    CHECK(sums_name_safe("..a"));
    CHECK(sums_name_safe("a/.hidden"));
    CHECK(sums_name_safe("caf\xc3\xa9"));
}

TEST(absolute_dotdot_drive_and_control_names_are_unsafe) {
    CHECK(!sums_name_safe("/etc/f"));
    CHECK(!sums_name_safe("\\f"));
    CHECK(!sums_name_safe(".."));
    CHECK(!sums_name_safe("../f"));
    CHECK(!sums_name_safe("a/../../f"));
    CHECK(!sums_name_safe("a/.."));
    CHECK(!sums_name_safe("a\\..\\f"));                // FatFs splits at '\' too
    CHECK(!sums_name_safe("a/.. /f"));                 // FatFs drops trailing spaces and dots
    CHECK(!sums_name_safe("..."));
    CHECK(!sums_name_safe(". ./f"));
    CHECK(!sums_name_safe("0:/f"));
    CHECK(!sums_name_safe("a\nb"));
    CHECK(!sums_name_safe("a\x01"));
}

TEST_MAIN(
    RUN(plain_names_are_shown_as_they_are);
    RUN(newline_and_backslash_are_escaped);
    RUN(other_control_characters_are_flagged_and_shown_as_marks);
    RUN(lines_split_at_newlines_in_any_block_size);
    RUN(an_empty_list_has_no_lines);
    RUN(a_line_of_the_limit_is_kept_and_a_longer_one_skipped);
    RUN(a_last_line_of_the_limit_without_newline_is_kept);
    RUN(gnu_text_and_binary_forms);
    RUN(hex_in_either_case);
    RUN(hex_of_the_wrong_length_or_not_hex_is_bad);
    RUN(separators_and_names_that_are_bad);
    RUN(blank_and_comment_lines_are_skipped);
    RUN(escaped_names_are_unescaped);
    RUN(bsd_form_with_the_matching_algorithm);
    RUN(bsd_form_that_is_bad);
    RUN(a_line_of_the_limit_parses);
    RUN(relative_names_are_safe);
    RUN(absolute_dotdot_drive_and_control_names_are_unsafe);
)
