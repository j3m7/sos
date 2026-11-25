// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/edit/editor.c: the editor's text, keys and screen. The screen output goes
// through a tiny terminal emulator, so the tests check what a terminal would show.

#include <stdlib.h>

#include "edit/editor.h"
#include "shell/keys.h"
#include "test.h"

// --- a tiny terminal: cursor movement, erase, and printable bytes (UTF-8 as one cell) ---------

#define T_ROWS EDITOR_ROWS_MAX
#define T_COLS EDITOR_COLS_MAX

static char screen[T_ROWS][T_COLS][5];      // one UTF-8 character per cell
static int t_row, t_col, t_rows, t_cols;
static bool t_cursor_on;
static size_t t_bytes;                      // output received in total

static void t_reset(int rows, int cols) {
    memset(screen, 0, sizeof(screen));
    t_row = t_col = 0;
    t_rows = rows;
    t_cols = cols;
    t_cursor_on = true;
    t_bytes = 0;
}

static void t_feed(const char *s, size_t n) {
    t_bytes += n;
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1b) {
            CHECK(i + 1 < n && s[i + 1] == '[');
            size_t j = i + 2;
            bool priv = j < n && s[j] == '?';
            if (priv)
                j++;
            int p[2] = { 0, 0 }, np = 0;
            while (j < n && ((s[j] >= '0' && s[j] <= '9') || s[j] == ';')) {
                if (s[j] == ';')
                    np++;
                else
                    p[np < 2 ? np : 1] = p[np < 2 ? np : 1] * 10 + (s[j] - '0');
                j++;
            }
            CHECK(j < n);
            char f = s[j];
            if (priv && p[0] == 25)
                t_cursor_on = f == 'h';
            else if (f == 'H') {
                t_row = (p[0] ? p[0] : 1) - 1;
                t_col = (p[1] ? p[1] : 1) - 1;
                CHECK(t_row < t_rows && t_col < t_cols);
            } else if (f == 'K') {
                for (int k = t_col; k < t_cols; k++)
                    screen[t_row][k][0] = '\0';
            } else if (f == 'J') {
                memset(screen, 0, sizeof(screen));
            } else if (f == 'm') {
                // attributes: ignored
            } else {
                CHECK(!"unexpected escape sequence");
            }
            i = j + 1;
            continue;
        }
        CHECK(c >= 0x20 && c != 0x7f);      // nothing else a terminal could act on
        CHECK(!(c >= 0x80 && c <= 0x9f) || i > 0);
        size_t l = c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;
        CHECK(i + l <= n);
        if (t_col < t_cols) {
            memcpy(screen[t_row][t_col], s + i, l);
            screen[t_row][t_col][l] = '\0';
        }
        t_col++;
        i += l;
    }
}

// Row r (0-based) as a string, trailing blanks dropped.
static const char *t_line(int r) {
    static char buf[T_COLS * 4 + 1];
    size_t n = 0;
    for (int k = 0; k < t_cols; k++) {
        const char *cell = screen[r][k][0] ? screen[r][k] : " ";
        size_t l = strlen(cell);
        memcpy(buf + n, cell, l);
        n += l;
    }
    while (n && buf[n - 1] == ' ')
        n--;
    buf[n] = '\0';
    return buf;
}

// --- helpers -------------------------------------------------------------------------------------

static editor_t ed;

static void open_text(const char *text, int rows, int cols) {
    CHECK(editor_init(&ed, "test.txt", text, strlen(text)));
    t_reset(rows, cols);
    editor_resize(&ed, rows, cols);
}

static void draw(void) {
    static char out[65536];
    size_t n = editor_draw(&ed, out, sizeof(out));
    t_feed(out, n);
    CHECK(editor_drawn(&ed));
}

static editor_action_t keys(const char *s) {
    editor_action_t a = EDITOR_NONE;
    for (; *s; s++)
        a = editor_key(&ed, (unsigned char)*s);
    return a;
}

static void key(int k) {
    editor_key(&ed, k);
}

// The text as it would be saved.
static const char *saved(void) {
    static char buf[EDITOR_MAX_TEXT * 2 + 1];
    size_t pos = 0, n = 0, got;
    while ((got = editor_export(&ed, &pos, buf + n, 7)) > 0)     // small pieces on purpose
        n += got;
    buf[n] = '\0';
    CHECK_EQ(n, editor_saved_len(&ed));
    return buf;
}

#define CHECK_LINE(r, expected) do {                                                        \
        const char *l_ = t_line(r);                                                         \
        test_checks++;                                                                      \
        if (strcmp(l_, (expected))) {                                                       \
            test_failures++;                                                                \
            fprintf(stderr, "%s:%d: %s: row %d is \"%s\", expected \"%s\"\n",                \
                    __FILE__, __LINE__, test_current, (r), l_, (expected));                 \
        }                                                                                   \
    } while (0)

// --- tests ---------------------------------------------------------------------------------------

TEST(shows_text_status_and_help) {
    open_text("hello\nworld\n", 6, 40);
    draw();
    CHECK_LINE(0, "hello");
    CHECK_LINE(1, "world");
    CHECK_LINE(2, "");                      // the empty line after the last newline
    CHECK_LINE(3, "~");
    CHECK(strstr(t_line(4), "test.txt") != NULL);
    CHECK(strstr(t_line(4), "line 1/3 col 1") != NULL);
    CHECK(strstr(t_line(5), "Ctrl-S save") != NULL);
    CHECK_EQ(t_row, 0);
    CHECK_EQ(t_col, 0);
    CHECK(t_cursor_on);
    editor_free(&ed);
}

TEST(typing_inserts_and_marks_modified) {
    open_text("ac", 5, 50);
    key(KEY_RIGHT);
    keys("b");
    CHECK(ed.modified);
    CHECK(!strcmp(saved(), "abc"));
    draw();
    CHECK_LINE(0, "abc");
    CHECK(strstr(t_line(3), "[modified]") != NULL);
    CHECK_EQ(t_col, 2);
    keys("\rX");
    CHECK(!strcmp(saved(), "ab\nXc"));
    editor_free(&ed);
}

TEST(backspace_and_delete) {
    open_text("abc\ndef", 5, 30);
    key(KEY_DOWN);                          // start of "def"
    keys("\x7f");                           // joins the lines
    CHECK(!strcmp(saved(), "abcdef"));
    key(KEY_DELETE);
    CHECK(!strcmp(saved(), "abcef"));
    keys("\x04");                           // Ctrl-D deletes forward too
    CHECK(!strcmp(saved(), "abcf"));
    key(KEY_HOME);
    keys("\x7f\b");                         // nothing before the start
    CHECK(!strcmp(saved(), "abcf"));
    key(KEY_END);
    key(KEY_DELETE);                        // nothing after the end
    CHECK(!strcmp(saved(), "abcf"));
    editor_free(&ed);
}

TEST(utf8_moves_and_deletes_whole_characters) {
    open_text("a\xc3\xa9" "b\xe2\x82\xac" "c", 5, 30);       // a é b € c
    key(KEY_RIGHT);
    key(KEY_RIGHT);                         // past é (2 bytes)
    CHECK_EQ(ed.cursor, 3);
    key(KEY_RIGHT);
    key(KEY_RIGHT);                         // past € (3 bytes)
    CHECK_EQ(ed.cursor, 7);
    key(KEY_LEFT);
    CHECK_EQ(ed.cursor, 4);
    keys("\x7f");                           // removes b
    key(KEY_LEFT);
    key(KEY_DELETE);                        // removes é
    CHECK(!strcmp(saved(), "a\xe2\x82\xac" "c"));
    draw();
    CHECK_LINE(0, "a\xe2\x82\xac" "c");
    keys("\xc3\xb1");                       // typed ñ arrives as two bytes
    CHECK(!strcmp(saved(), "a\xc3\xb1\xe2\x82\xac" "c"));
    editor_free(&ed);
}

TEST(dangerous_bytes_show_as_question_marks) {
    // ESC sequence, C1 as a raw byte and UTF-8-encoded, invalid UTF-8, bidi override, CR, DEL.
    open_text("a\x1b[2Jb\x9b" "c\xc2\x9b" "d\xff" "e\xe2\x80\xae" "f\rg\x7fh", 5, 60);
    draw();                                 // t_feed() CHECKs that no control reaches it
    CHECK_LINE(0, "a?[2Jb?c?d?e?f?g?h");
    editor_free(&ed);
}

TEST(tabs_expand_to_tab_stops) {
    open_text("\tx\nab\ty", 5, 30);
    draw();
    CHECK_LINE(0, "        x");
    CHECK_LINE(1, "ab      y");
    key(KEY_RIGHT);
    draw();
    CHECK_EQ(t_col, 8);
    key(KEY_DOWN);                          // column 8 on "ab\ty": just after the tab
    CHECK_EQ(ed.cursor, 2 + 1 + 3);
    editor_free(&ed);
}

TEST(up_and_down_keep_the_column) {
    open_text("long line here\nab\nanother long line", 6, 40);
    for (int i = 0; i < 10; i++)
        key(KEY_RIGHT);
    key(KEY_DOWN);                          // short line: end of "ab"
    CHECK_EQ(ed.cursor, 15 + 2);
    key(KEY_DOWN);                          // back to column 10
    CHECK_EQ(ed.cursor, 18 + 10);
    key(KEY_UP);
    key(KEY_UP);
    CHECK_EQ(ed.cursor, 10);
    key(KEY_UP);                            // first line: stays
    CHECK_EQ(ed.cursor, 10);
    editor_free(&ed);
}

TEST(scrolls_down_and_up) {
    char text[2000] = "";
    for (int i = 1; i <= 50; i++)
        sprintf(text + strlen(text), "line %d\n", i);
    open_text(text, 7, 30);                 // 5 text rows
    draw();
    CHECK_LINE(0, "line 1");
    CHECK_LINE(4, "line 5");
    for (int i = 0; i < 5; i++)
        key(KEY_DOWN);
    draw();
    CHECK_LINE(0, "line 2");
    CHECK_LINE(4, "line 6");
    CHECK_EQ(t_row, 4);
    key(KEY_PGDN);
    draw();
    CHECK_LINE(4, "line 10");
    CHECK(strstr(t_line(5), "line 10/51") != NULL);
    key(KEY_PGUP);
    key(KEY_PGUP);
    key(KEY_PGUP);
    draw();
    CHECK_LINE(0, "line 1");
    CHECK_EQ(t_row, 0);
    editor_free(&ed);
}

TEST(long_lines_scroll_sideways) {
    char text[300];
    for (int i = 0; i < 100; i++)
        text[i] = (char)('0' + i % 10);
    text[100] = '\0';
    open_text(text, 5, 20);
    key(KEY_END);
    draw();
    CHECK_EQ(t_col, 19);
    CHECK_LINE(0, "1234567890123456789");          // columns 81-99, cursor past the end
    key(KEY_HOME);
    draw();
    CHECK_LINE(0, "01234567890123456789");
    CHECK_EQ(t_col, 0);
    editor_free(&ed);
}

TEST(only_changed_rows_are_redrawn) {
    char text[2000] = "";
    for (int i = 1; i <= 20; i++)
        sprintf(text + strlen(text), "line %d\n", i);
    open_text(text, 24, 80);
    draw();
    size_t full = t_bytes;
    t_bytes = 0;
    keys("x");                              // one text row and the status row change
    draw();
    CHECK(t_bytes < 200);                   // vs. the whole screen
    CHECK(full > 400);
    CHECK_LINE(0, "xline 1");
    t_bytes = 0;
    key(KEY_RIGHT);                         // the status row (column) and cursor only
    draw();
    CHECK(t_bytes < 120);
    editor_free(&ed);
}

TEST(drawing_fits_the_space_given) {
    char text[4000] = "";
    for (int i = 1; i <= 40; i++)
        sprintf(text + strlen(text), "this is line number %d of the test text\n", i);
    open_text(text, 30, 80);
    static char out[120];                   // a status row (~100 bytes) or two text rows
    int calls = 0;
    while (!editor_drawn(&ed)) {
        size_t n = editor_draw(&ed, out, sizeof(out));
        CHECK(n <= sizeof(out));
        t_feed(out, n);
        CHECK(++calls < 100);
    }
    CHECK(calls > 3);
    CHECK_LINE(27, "this is line number 28 of the test text");
    CHECK(t_cursor_on);
    CHECK_EQ(editor_draw(&ed, out, sizeof(out)), 0);     // nothing more to do
    editor_free(&ed);
}

TEST(change_during_a_frame_restarts_it) {
    open_text("one\ntwo\nthree\n", 10, 40);
    static char out[64];                    // a row or two per call
    t_feed(out, editor_draw(&ed, out, sizeof(out)));
    CHECK(!editor_drawn(&ed));
    keys("X");
    while (!editor_drawn(&ed))
        t_feed(out, editor_draw(&ed, out, sizeof(out)));
    CHECK_LINE(0, "Xone");
    CHECK_LINE(2, "three");
    CHECK_EQ(t_col, 1);
    editor_free(&ed);
}

TEST(save_and_quit) {
    open_text("abc", 5, 30);
    CHECK_EQ(keys("\x11"), EDITOR_QUIT);    // unmodified: Ctrl-Q quits
    keys("x");
    CHECK_EQ(keys("\x11"), EDITOR_NONE);    // modified: asks first
    draw();
    CHECK(strstr(t_line(4), "unsaved changes") != NULL);
    CHECK_EQ(keys("\x11"), EDITOR_QUIT);
    keys("y");
    CHECK_EQ(keys("\x11"), EDITOR_NONE);
    keys("z");                              // another key disarms it
    CHECK_EQ(keys("\x11"), EDITOR_NONE);
    CHECK_EQ(keys("\x13"), EDITOR_SAVE);    // Ctrl-S
    CHECK_EQ(keys("\x0f"), EDITOR_SAVE);    // Ctrl-O
    editor_saved(&ed, false, "write failed");
    CHECK(ed.modified);
    editor_saved(&ed, true, "saved");
    CHECK(!ed.modified);
    CHECK_EQ(keys("\x18"), EDITOR_QUIT);    // Ctrl-X quits too
    editor_free(&ed);
}

TEST(ctrl_l_redraws_and_asks_for_size) {
    open_text("abc", 5, 30);
    draw();
    CHECK_EQ(keys("\x0c"), EDITOR_RESIZE);
    CHECK(!editor_drawn(&ed));
    editor_resize(&ed, 8, 50);
    t_reset(8, 50);
    draw();
    CHECK_LINE(0, "abc");
    CHECK(strstr(t_line(6), "test.txt") != NULL);
    editor_resize(&ed, 1, 1000);            // clamped
    CHECK_EQ(ed.rows, EDITOR_ROWS_MIN);
    CHECK_EQ(ed.cols, EDITOR_COLS_MAX);
    editor_free(&ed);
}

TEST(crlf_files_keep_their_line_ends) {
    open_text("a\r\nb\r\n", 5, 30);
    CHECK(ed.crlf);
    CHECK_EQ(editor_len(&ed), 4);           // shown without the CRs
    draw();
    CHECK_LINE(0, "a");
    key(KEY_END);
    keys("\rc");
    CHECK(!strcmp(saved(), "a\r\nc\r\nb\r\n"));
    editor_free(&ed);

    open_text("a\r\nb\n", 5, 30);           // mixed: kept as they are
    CHECK(!ed.crlf);
    CHECK(!strcmp(saved(), "a\r\nb\n"));
    editor_free(&ed);
}

TEST(find_moves_to_the_next_match) {
    open_text("cat dog\nbird dog\ncat", 6, 40);
    keys("\x06" "dog\r");                   // Ctrl-F dog
    CHECK_EQ(ed.cursor, 4);
    keys("\x06\r");                         // empty: the last search again
    CHECK_EQ(ed.cursor, 13);
    keys("\x06\r");                         // wraps around
    CHECK_EQ(ed.cursor, 4);
    draw();
    CHECK(strstr(t_line(5), "wrapped") != NULL);
    keys("\x06" "zebra\r");
    CHECK_EQ(ed.cursor, 4);
    draw();
    CHECK(strstr(t_line(5), "not found") != NULL);
    keys("\x06" "ca");
    draw();
    CHECK_LINE(5, "Search: ca");
    CHECK_EQ(t_row, 5);
    CHECK_EQ(t_col, 10);
    keys("x\x7f\x03");                      // edit the query, then cancel with Ctrl-C
    CHECK(!ed.finding);
    CHECK_EQ(ed.cursor, 4);
    CHECK(!ed.modified);                    // search keys never touched the text
    editor_free(&ed);
}

TEST(size_limit) {
    char *big = malloc(EDITOR_MAX_TEXT + 1);
    memset(big, 'a', EDITOR_MAX_TEXT + 1);
    CHECK(!editor_init(&ed, "big", big, EDITOR_MAX_TEXT + 1));
    editor_free(&ed);
    CHECK(editor_init(&ed, "full", big, EDITOR_MAX_TEXT));
    keys("b");                              // can't grow
    CHECK_EQ(editor_len(&ed), EDITOR_MAX_TEXT);
    CHECK(!ed.modified);
    CHECK(strstr(ed.msg, "32 KB") != NULL);
    editor_free(&ed);
    free(big);
}

TEST(grows_while_typing) {
    open_text("", 5, 30);
    for (int i = 0; i < 5000; i++)
        keys(i % 50 == 49 ? "\r" : "q");
    CHECK_EQ(editor_len(&ed), 5000);
    CHECK(ed.cap >= 5000);
    CHECK_EQ(saved()[49], '\n');
    draw();
    CHECK(strstr(t_line(3), "line 101/101") != NULL);
    editor_free(&ed);
}

TEST(log_messages_waiting_show_in_status) {
    open_text("abc", 5, 60);
    draw();
    editor_set_log_waiting(&ed, 3);
    CHECK(!editor_drawn(&ed));
    draw();
    CHECK(strstr(t_line(3), "3 log messages held (dmesg)") != NULL);
    editor_set_log_waiting(&ed, 3);         // unchanged: nothing to draw
    CHECK(editor_drawn(&ed));
    editor_free(&ed);
}

TEST(random_keys_keep_invariants) {
    srand(1234);
    open_text("start\n\tof\xc3\xa9 text\n", 8, 30);
    static const int pool[] = {
        'a', 'z', ' ', '\r', '\t', 0x7f, 0x04, 0xc3, 0xa9, 0xe2, 0x80, 0xae, 0x1b,
        KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_DELETE,
    };
    static char out[600];
    for (int i = 0; i < 20000; i++) {
        key(pool[rand() % (int)(sizeof(pool) / sizeof(pool[0]))]);
        CHECK(ed.cursor <= editor_len(&ed));
        if (i % 7 == 0)
            while (!editor_drawn(&ed))
                t_feed(out, editor_draw(&ed, out, sizeof(out)));
    }
    editor_free(&ed);
}

TEST_MAIN(
    RUN(shows_text_status_and_help);
    RUN(typing_inserts_and_marks_modified);
    RUN(backspace_and_delete);
    RUN(utf8_moves_and_deletes_whole_characters);
    RUN(dangerous_bytes_show_as_question_marks);
    RUN(tabs_expand_to_tab_stops);
    RUN(up_and_down_keep_the_column);
    RUN(scrolls_down_and_up);
    RUN(long_lines_scroll_sideways);
    RUN(only_changed_rows_are_redrawn);
    RUN(drawing_fits_the_space_given);
    RUN(change_during_a_frame_restarts_it);
    RUN(save_and_quit);
    RUN(ctrl_l_redraws_and_asks_for_size);
    RUN(crlf_files_keep_their_line_ends);
    RUN(find_moves_to_the_next_match);
    RUN(size_limit);
    RUN(grows_while_typing);
    RUN(log_messages_waiting_show_in_status);
    RUN(random_keys_keep_invariants);
)
