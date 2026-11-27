// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/shell/keys.c: terminal key decoding.

#include "shell/keys.h"
#include "test.h"

// Feed a string; collect the keys it produces (KEYS_NONE skipped) into out[], return the count.
static int feed(keys_t *k, const char *s, int *out, int max) {
    int n = 0;
    for (; *s; s++) {
        int key = keys_feed(k, (unsigned char)*s);
        if (key != KEYS_NONE && n < max)
            out[n++] = key;
    }
    return n;
}

// The one key a complete sequence produces, from a fresh decoder.
static int one(const char *s) {
    keys_t k = KEYS_INIT;
    int out[8];
    int n = feed(&k, s, out, 8);
    return n == 1 ? out[0] : n == 0 ? KEYS_NONE : -2;
}

TEST(plain_bytes_pass_through) {
    keys_t k = KEYS_INIT;
    for (int c = 0; c < 256; c++)
        if (c != 0x1b)
            CHECK_EQ(keys_feed(&k, (unsigned char)c), c);
}

TEST(csi_arrows_home_end) {
    CHECK_EQ(one("\x1b[A"), KEY_UP);
    CHECK_EQ(one("\x1b[B"), KEY_DOWN);
    CHECK_EQ(one("\x1b[C"), KEY_RIGHT);
    CHECK_EQ(one("\x1b[D"), KEY_LEFT);
    CHECK_EQ(one("\x1b[H"), KEY_HOME);
    CHECK_EQ(one("\x1b[F"), KEY_END);
}

TEST(ss3_arrows_home_end) {
    CHECK_EQ(one("\x1bOA"), KEY_UP);
    CHECK_EQ(one("\x1bOB"), KEY_DOWN);
    CHECK_EQ(one("\x1bOC"), KEY_RIGHT);
    CHECK_EQ(one("\x1bOD"), KEY_LEFT);
    CHECK_EQ(one("\x1bOH"), KEY_HOME);
    CHECK_EQ(one("\x1bOF"), KEY_END);
    CHECK_EQ(one("\x1bOP"), KEYS_NONE);             // F1
}

TEST(vt220_tilde_keys) {
    CHECK_EQ(one("\x1b[1~"), KEY_HOME);
    CHECK_EQ(one("\x1b[7~"), KEY_HOME);             // rxvt
    CHECK_EQ(one("\x1b[2~"), KEY_INSERT);
    CHECK_EQ(one("\x1b[3~"), KEY_DELETE);
    CHECK_EQ(one("\x1b[4~"), KEY_END);
    CHECK_EQ(one("\x1b[8~"), KEY_END);
    CHECK_EQ(one("\x1b[5~"), KEY_PGUP);
    CHECK_EQ(one("\x1b[6~"), KEY_PGDN);
    CHECK_EQ(one("\x1b[15~"), KEYS_NONE);           // F5
    CHECK_EQ(one("\x1b[200~"), KEYS_NONE);          // bracketed paste start
    CHECK_EQ(one("\x1b[~"), KEYS_NONE);
}

TEST(modifiers_are_ignored) {
    CHECK_EQ(one("\x1b[1;2C"), KEY_RIGHT);          // Shift+Right
    CHECK_EQ(one("\x1b[1;2A"), KEY_UP);             // Shift+Up
    CHECK_EQ(one("\x1b[1;5A"), KEY_UP);             // Ctrl+Up
    CHECK_EQ(one("\x1b[3;5~"), KEY_DELETE);         // Ctrl+Delete
    CHECK_EQ(one("\x1b[1;3H"), KEY_HOME);           // Alt+Home
    CHECK_EQ(one("\x1b[5;5~"), KEY_PGUP);           // Ctrl+PgUp
}

TEST(word_and_text_keys) {
    CHECK_EQ(one("\x1b[1;5C"), KEY_WORD_RIGHT);     // Ctrl+Right (xterm)
    CHECK_EQ(one("\x1b[1;5D"), KEY_WORD_LEFT);
    CHECK_EQ(one("\x1b[1;3C"), KEY_WORD_RIGHT);     // Alt+Right
    CHECK_EQ(one("\x1b[1;7D"), KEY_WORD_LEFT);      // Ctrl+Alt+Left
    CHECK_EQ(one("\x1b[1;6C"), KEY_WORD_RIGHT);     // Ctrl+Shift+Right
    CHECK_EQ(one("\x1bOc"), KEY_WORD_RIGHT);        // rxvt
    CHECK_EQ(one("\x1bOd"), KEY_WORD_LEFT);
    CHECK_EQ(one("\x1b" "b"), KEY_WORD_LEFT);          // Alt+B, Alt+F (macOS Option+arrows)
    CHECK_EQ(one("\x1b" "f"), KEY_WORD_RIGHT);
    CHECK_EQ(one("\x1b[1;5H"), KEY_CTRL_HOME);      // Ctrl+Home, three ways
    CHECK_EQ(one("\x1b[1;5~"), KEY_CTRL_HOME);
    CHECK_EQ(one("\x1b[7^"), KEY_CTRL_HOME);
    CHECK_EQ(one("\x1b[1;5F"), KEY_CTRL_END);
    CHECK_EQ(one("\x1b[8;5~"), KEY_CTRL_END);
    CHECK_EQ(one("\x1b[8^"), KEY_CTRL_END);
    CHECK_EQ(one("\x1b[2^"), KEYS_NONE);            // rxvt Ctrl+Insert: nothing
    keys_t k = KEYS_INIT;
    int out[4];
    CHECK_EQ(feed(&k, "\x1b" "bx", out, 4), 2);        // the key after Alt+B is its own
    CHECK_EQ(out[0], KEY_WORD_LEFT);
    CHECK_EQ(out[1], 'x');
}

TEST(cursor_position_report) {
    keys_t k = KEYS_INIT;
    int out[4];
    CHECK_EQ(feed(&k, "\x1b[24;80R", out, 4), 1);
    CHECK_EQ(out[0], KEY_CURSOR_POS);
    CHECK_EQ(k.param[0], 24);
    CHECK_EQ(k.param[1], 80);
    CHECK_EQ(feed(&k, "\x1b[123;4567R", out, 4), 1);
    CHECK_EQ(k.param[0], 123);
    CHECK_EQ(k.param[1], 4567);
    CHECK_EQ(feed(&k, "\x1b[99999;99999R", out, 4), 1);    // capped, not wrapped
    CHECK_EQ(k.param[0], 9999);
    CHECK_EQ(k.param[1], 9999);
    CHECK_EQ(one("\x1b[5R"), KEYS_NONE);            // one parameter: not a position
    CHECK_EQ(one("\x1b[1;2;3R"), KEYS_NONE);        // three: neither
    CHECK_EQ(one("\x1b[?1;2R"), KEYS_NONE);         // private (DECXCPR-like): no
    CHECK_EQ(one("\x1b[;R"), KEYS_NONE);            // rows and columns count from 1
    CHECK_EQ(one("\x1b[0;0R"), KEYS_NONE);
    CHECK_EQ(one("\x1b[5;0R"), KEYS_NONE);
    CHECK_EQ(one("\x1b[1;1$R"), KEYS_NONE);         // intermediate byte: some other report
}

TEST(private_sequences_are_not_keys) {
    CHECK_EQ(one("\x1b[?1A"), KEYS_NONE);
    CHECK_EQ(one("\x1b[<0;10;5M"), KEYS_NONE);      // SGR mouse report
    CHECK_EQ(one("\x1b[>0;95;0c"), KEYS_NONE);      // device attributes answer
}

TEST(alt_key_is_dropped) {
    keys_t k = KEYS_INIT;
    int out[4];
    CHECK_EQ(feed(&k, "\x1bxy", out, 4), 1);
    CHECK_EQ(out[0], 'y');
    CHECK_EQ(feed(&k, "\x1b\xc3\xa9z", out, 4), 1);        // Alt+e-acute: all of it
    CHECK_EQ(out[0], 'z');
    CHECK_EQ(feed(&k, "\x1b\xe2\x82\xacz", out, 4), 1);   // Alt+euro sign (3 bytes)
    CHECK_EQ(out[0], 'z');
    CHECK_EQ(feed(&k, "\x1b\xf0\x9f\x98\x80z", out, 4), 1);   // 4 bytes
    CHECK_EQ(out[0], 'z');
    CHECK_EQ(feed(&k, "\x1b\xc3z", out, 4), 1);             // cut short: the byte counts
    CHECK_EQ(out[0], 'z');
    CHECK_EQ(feed(&k, "\x1b\xc3\x1b[A", out, 4), 1);       // or a new sequence
    CHECK_EQ(out[0], KEY_UP);
}

TEST(intermediate_bytes_are_not_keys) {
    CHECK_EQ(one("\x1b[2 A"), KEYS_NONE);
    CHECK_EQ(one("\x1b[!p"), KEYS_NONE);            // soft reset
    CHECK_EQ(one("\x1b[A"), KEY_UP);
}

TEST(control_character_abandons_sequence) {
    keys_t k = KEYS_INIT;
    int out[4];
    CHECK_EQ(feed(&k, "\x1b\r", out, 4), 1);        // stray ESC, then Enter
    CHECK_EQ(out[0], '\r');
    CHECK_EQ(feed(&k, "\x1b[1;\x03", out, 4), 1);   // Ctrl-C inside a CSI
    CHECK_EQ(out[0], 0x03);
    CHECK_EQ(feed(&k, "\x1bO\x7f", out, 4), 0);     // DEL isn't a control character: SS3 byte
    CHECK_EQ(feed(&k, "a", out, 4), 1);             // back to plain bytes
    CHECK_EQ(out[0], 'a');
}

TEST(escape_restarts_sequence) {
    keys_t k = KEYS_INIT;
    int out[4];
    CHECK_EQ(feed(&k, "\x1b[1\x1b[B", out, 4), 1);  // ESC inside a CSI starts over
    CHECK_EQ(out[0], KEY_DOWN);
}

TEST(split_byte_at_a_time) {
    keys_t k = KEYS_INIT;
    const char *s = "a\x1b[3;5~b\x1bOHc\x1b[12;34R";
    int got[8], n = 0;
    for (const char *p = s; *p; p++) {
        int key = keys_feed(&k, (unsigned char)*p);
        if (key != KEYS_NONE)
            got[n++] = key;
    }
    CHECK_EQ(n, 6);
    CHECK_EQ(got[0], 'a');
    CHECK_EQ(got[1], KEY_DELETE);
    CHECK_EQ(got[2], 'b');
    CHECK_EQ(got[3], KEY_HOME);
    CHECK_EQ(got[4], 'c');
    CHECK_EQ(got[5], KEY_CURSOR_POS);
    CHECK_EQ(k.param[0], 12);
    CHECK_EQ(k.param[1], 34);
}

TEST(state_resets_between_sequences) {
    keys_t k = KEYS_INIT;
    int out[4];
    CHECK_EQ(feed(&k, "\x1b[?5A", out, 4), 0);
    CHECK_EQ(feed(&k, "\x1b[A", out, 4), 1);        // private flag doesn't carry over
    CHECK_EQ(out[0], KEY_UP);
    CHECK_EQ(feed(&k, "\x1b[12;34R\x1b[R", out, 4), 1);    // nor parameters
}

TEST(long_garbage_sequence_ends) {
    keys_t k = KEYS_INIT;
    int out[4];
    char s[600] = "\x1b[";
    memset(s + 2, ';', 300);
    memset(s + 302, '9', 290);
    strcpy(s + 592, "Ax");
    CHECK_EQ(feed(&k, s, out, 4), 2);               // the A ends it, an arrow with junk modifiers
    CHECK_EQ(out[0], KEY_UP);
    CHECK_EQ(out[1], 'x');
}

TEST_MAIN(
    RUN(plain_bytes_pass_through);
    RUN(csi_arrows_home_end);
    RUN(ss3_arrows_home_end);
    RUN(vt220_tilde_keys);
    RUN(modifiers_are_ignored);
    RUN(word_and_text_keys);
    RUN(cursor_position_report);
    RUN(private_sequences_are_not_keys);
    RUN(alt_key_is_dropped);
    RUN(intermediate_bytes_are_not_keys);
    RUN(control_character_abandons_sequence);
    RUN(escape_restarts_sequence);
    RUN(split_byte_at_a_time);
    RUN(state_resets_between_sequences);
    RUN(long_garbage_sequence_ends);
)
