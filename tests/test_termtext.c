// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/shell/termtext.c: making outside text safe to print on a terminal.

#include <stdint.h>
#include <stdlib.h>

#include "shell/termtext.h"
#include "test.h"

static char out[4096];

// Filter s in pieces of `step` bytes (0: all at once), then flush. Returns the result.
static const char *filter(const char *s, size_t len, size_t step) {
    termtext_t t = TERMTEXT_INIT;
    size_t o = 0, at = 0;
    while (at < len) {
        size_t n = step ? step : len;
        if (n > len - at)
            n = len - at;
        size_t w = termtext_filter(&t, (const uint8_t *)s + at, n, out + o);
        CHECK(w <= n + 3);
        o += w;
        at += n;
    }
    o += termtext_flush(&t, out + o);
    out[o] = '\0';
    return out;
}

// The same result whole and in every piece size up to 5.
static void check(const char *in, size_t len, const char *want) {
    for (size_t step = 0; step <= 5; step++) {
        const char *got = filter(in, len, step);
        if (strcmp(got, want))
            fprintf(stderr, "  step %zu: got \"%s\", want \"%s\"\n", step, got, want);
        CHECK(!strcmp(got, want));
    }
}

#define CHECK_TEXT(in, want) check((in), sizeof(in) - 1, (want))

TEST(ascii) {
    CHECK_TEXT("hello, world\n\ttab", "hello, world\n\ttab");
    CHECK_TEXT("crlf\r\nline", "crlf\nline");
    CHECK_TEXT("", "");
    CHECK_TEXT(" ~", " ~");
}

TEST(c0_and_del) {
    CHECK_TEXT("a\x1b[31mred", "a.[31mred");                     // ESC
    CHECK_TEXT("\x07\x08\x00z", "...z");                          // BEL, BS, NUL
    CHECK_TEXT("x\x7fy", "x.y");                                  // DEL
}

TEST(c1_raw) {
    // 0x80-0x9F alone, including 0x9B (CSI on 8-bit terminals) and 0x9D (OSC).
    CHECK_TEXT("a\x9b" "31mb", "a.31mb");
    CHECK_TEXT("\x80\x9d\x9f", "...");
}

TEST(c1_utf8) {
    // U+0080-U+009F encoded as UTF-8: C2 80 .. C2 9F.
    CHECK_TEXT("a\xc2\x9b" "31mb", "a.31mb");
    CHECK_TEXT("\xc2\x80\xc2\x9f", "..");
    // U+00A0 (no-break space) and up pass.
    CHECK_TEXT("\xc2\xa0\xc2\xa9", "\xc2\xa0\xc2\xa9");
}

TEST(utf8_valid) {
    CHECK_TEXT("caf\xc3\xa9", "caf\xc3\xa9");                     // é
    CHECK_TEXT("\xe2\x82\xac 5", "\xe2\x82\xac 5");               // €
    CHECK_TEXT("\xe6\x97\xa5\xe6\x9c\xac", "\xe6\x97\xa5\xe6\x9c\xac");    // 日本
    CHECK_TEXT("\xf0\x9f\x98\x80!", "\xf0\x9f\x98\x80!");         // emoji
    CHECK_TEXT("\xf4\x8f\xbf\xbf", "\xf4\x8f\xbf\xbf");           // U+10FFFF
}

TEST(utf8_invalid) {
    CHECK_TEXT("\xc0\xaf", "..");                 // overlong '/', both bytes bad
    CHECK_TEXT("\xc1\xbf", "..");
    CHECK_TEXT("\xe0\x80\xaf", "...");            // overlong 3-byte: E0 then 80 refused
    CHECK_TEXT("\xed\xa0\x80", "...");            // surrogate U+D800
    CHECK_TEXT("\xf0\x80\x80\xaf", "....");       // overlong 4-byte
    CHECK_TEXT("\xf4\x90\x80\x80", "....");       // above U+10FFFF
    CHECK_TEXT("\xf5\x80", "..");
    CHECK_TEXT("\xff\xfe", "..");
    CHECK_TEXT("\xbf", ".");                      // stray continuation byte
    // A sequence broken by an ASCII byte: one '.', then the byte itself.
    CHECK_TEXT("\xe2\x82x", ".x");
    CHECK_TEXT("\xc3\n", ".\n");
    // ...or by ESC: still made safe.
    CHECK_TEXT("\xc3\x1b[0m", "..[0m");
    // ...or by another lead byte, which starts afresh.
    CHECK_TEXT("\xc3\xc3\xa9", ".\xc3\xa9");
    // Cut off at the end.
    CHECK_TEXT("ok\xe2\x82", "ok.");
    CHECK_TEXT("\xf0", ".");
}

TEST(hidden) {
    // Bidi controls: RLO would show "abc" + "txt.exe" reversed as "exe.txt".
    CHECK_TEXT("abc\xe2\x80\xaetxt.exe", "abc.txt.exe");                 // U+202E RLO
    CHECK_TEXT("\xe2\x80\xaa\xe2\x80\xab\xe2\x80\xac\xe2\x80\xad", "....");  // U+202A-D
    CHECK_TEXT("\xe2\x81\xa6\xe2\x81\xa7\xe2\x81\xa8\xe2\x81\xa9", "....");  // U+2066-9
    CHECK_TEXT("\xe2\x80\x8e\xe2\x80\x8f\xd8\x9c", "...");          // LRM, RLM, ALM
    // Zero-width and invisible.
    CHECK_TEXT("pay\xe2\x80\x8bpal", "pay.pal");                        // U+200B ZWSP
    CHECK_TEXT("\xe2\x81\xa0\xe2\x81\xa4", "..");                     // U+2060, U+2064
    CHECK_TEXT("\xef\xbb\xbfhi", ".hi");                                 // U+FEFF BOM
    CHECK_TEXT("\xe2\x80\xa8\xe2\x80\xa9", "..");                     // U+2028, U+2029
    CHECK_TEXT("\xf3\xa0\x80\x81\xf3\xa0\x81\xbf", "..");           // U+E0001, U+E007F
    // Neighbours pass, and so do ZWNJ and ZWJ.
    CHECK_TEXT("\xe2\x80\x8a\xe2\x80\x90", "\xe2\x80\x8a\xe2\x80\x90");   // U+200A, U+2010
    CHECK_TEXT("\xe2\x80\x8c\xe2\x80\x8d", "\xe2\x80\x8c\xe2\x80\x8d");   // U+200C, U+200D
    CHECK_TEXT("\xe2\x80\xaf\xe2\x81\xa5", "\xe2\x80\xaf\xe2\x81\xa5");   // U+202F, U+2065
    CHECK_TEXT("\xef\xbb\xbe\xf3\xa0\x82\x80", "\xef\xbb\xbe\xf3\xa0\x82\x80");  // U+FEFE, U+E0080
}

// Random bytes in random pieces: the output never contains a C0 control other than \n and
// \t, DEL, or a C1 control, raw or UTF-8-encoded; and it never grows beyond the input.
TEST(fuzz) {
    static uint8_t in[512];
    unsigned seed = 4242;
    for (int iter = 0; iter < 20000; iter++) {
        size_t len = 1 + (size_t)(iter % 64);
        for (size_t i = 0; i < len; i++) {
            seed = seed * 1103515245u + 12345u;
            uint8_t b = (uint8_t)(seed >> 16);
            // Bias toward interesting bytes.
            if ((seed >> 8) % 4 == 0)
                b = (uint8_t[]){0x1b, 0x9b, 0xc2, 0xe2, 0xf0, 0x80, 0xbf, '\r', '\n'}[(seed >> 4) % 9];
            in[i] = b;
        }
        termtext_t t = TERMTEXT_INIT;
        size_t o = 0, at = 0, step = 1 + (size_t)(iter % 7);
        while (at < len) {
            size_t n = step < len - at ? step : len - at;
            o += termtext_filter(&t, in + at, n, out + o);
            at += n;
        }
        o += termtext_flush(&t, out + o);
        CHECK(o <= len);
        for (size_t i = 0; i < o; i++) {
            uint8_t c = (uint8_t)out[i];
            CHECK(c == '\n' || c == '\t' || (c >= 0x20 && c != 0x7f));
            CHECK(!(c >= 0x80 && c <= 0x9f && (i == 0 || (uint8_t)out[i - 1] == 0xc2 ||
                                               (uint8_t)out[i - 1] < 0x80)));
        }
    }
}

TEST_MAIN(
    RUN(ascii);
    RUN(c0_and_del);
    RUN(c1_raw);
    RUN(c1_utf8);
    RUN(utf8_valid);
    RUN(utf8_invalid);
    RUN(hidden);
    RUN(fuzz);
)
