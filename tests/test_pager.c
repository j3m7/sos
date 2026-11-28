// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/edit/pager.c: the pager's window, keys and screen. The screen output goes
// through the tiny terminal in termemu.h, so the tests check what a terminal would show.

#include <stdio.h>
#include <stdlib.h>

#include "edit/pager.h"
#include "shell/keys.h"
#include "termemu.h"
#include "test.h"

// --- helpers -------------------------------------------------------------------------------------

static pager_t pg;

// A source over a text in RAM that counts its reads, and can fail from an offset on.
typedef struct {
    const char *text;
    size_t len;
    int reads;
    size_t largest;                         // the biggest read asked for
    size_t fail_from;                       // reads reaching this offset fail (0: never)
} src_t;

static src_t source;

static size_t src_read(void *ctx, size_t off, char *buf, size_t n) {
    src_t *s = ctx;
    s->reads++;
    if (n > s->largest)
        s->largest = n;
    if (s->fail_from && off + n > s->fail_from)
        return off < s->fail_from ? s->fail_from - off : 0;
    CHECK(off + n <= s->len);
    memcpy(buf, s->text + off, n);
    return n;
}

static void open_text(const char *text, int rows, int cols, size_t window) {
    source = (src_t){ .text = text, .len = strlen(text) };
    pager_source_t src = { .read = src_read, .ctx = &source, .size = source.len };
    CHECK(pager_init(&pg, "test.txt", src, window));
    t_reset(rows, cols);
    pager_resize(&pg, rows, cols);
}

static void draw(void) {
    static char out[65536];
    while (!screen_drawn(&pg.scr)) {
        size_t n = screen_draw(&pg.scr, out, sizeof(out));
        CHECK(n > 0);
        t_feed(out, n);
    }
}

static pager_action_t key(int k) {
    return pager_key(&pg, k);
}

static void keys(const char *s) {
    for (; *s; s++)
        pager_key(&pg, (unsigned char)*s);
}

// The status row (the last one).
static const char *status(void) {
    return t_line(t_rows - 1);
}

// 1..n lines of "line <i> ..." text.
static char *numbered(int n, const char *tail) {
    char *text = malloc((size_t)n * 64 + 1);
    size_t o = 0;
    for (int i = 1; i <= n; i++)
        o += (size_t)sprintf(text + o, "line %d%s\n", i, tail);
    return text;
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

// Row r's reverse-video cells, as t_rev_line() shows them ('^' under each).
#define CHECK_REV(r, expected) do {                                                         \
        const char *l_ = t_rev_line(r);                                                     \
        test_checks++;                                                                      \
        if (strcmp(l_, (expected))) {                                                       \
            test_failures++;                                                                \
            fprintf(stderr, "%s:%d: %s: row %d inverted \"%s\", expected \"%s\"\n",         \
                    __FILE__, __LINE__, test_current, (r), l_, (expected));                 \
        }                                                                                   \
    } while (0)

// --- tests ---------------------------------------------------------------------------------------

TEST(shows_text_and_status) {
    open_text("one\ntwo\nthree\n", 6, 40, 0);
    draw();
    CHECK_LINE(0, "one");
    CHECK_LINE(1, "two");
    CHECK_LINE(2, "three");
    CHECK_LINE(3, "~");                     // no empty line after the last newline
    CHECK_LINE(4, "~");
    CHECK(strstr(status(), "q quits") != NULL);     // the first status is the hint
    key('j');                               // nothing to scroll: the text fits
    draw();
    CHECK_LINE(0, "one");
    CHECK(strstr(status(), "test.txt") != NULL);
    CHECK(strstr(status(), "lines 1-3/3 END") != NULL);
    CHECK(!t_cursor_on);                    // no cursor while reading
    pager_free(&pg);
}

TEST(no_newline_at_the_end_and_empty_text) {
    open_text("one\ntwo", 5, 40, 0);
    key('j');
    draw();
    CHECK_LINE(0, "one");
    CHECK_LINE(1, "two");
    CHECK(strstr(status(), "lines 1-2/2 END") != NULL);
    pager_free(&pg);

    open_text("", 5, 40, 0);
    key('j');
    draw();
    CHECK_LINE(0, "~");
    CHECK(strstr(status(), "empty") != NULL);
    pager_free(&pg);
}

TEST(pages_lines_and_ends) {
    char *text = numbered(100, "");
    open_text(text, 12, 40, 0);                // 11 text rows
    key('h');
    draw();
    CHECK(strstr(status(), "Space/b page") != NULL);

    key(' ');
    draw();
    CHECK_LINE(0, "line 12");
    CHECK(strstr(status(), "lines 12-22") != NULL);
    CHECK(strstr(status(), "/100") == NULL);        // the total isn't known yet
    CHECK(strstr(status(), "%") != NULL);
    key('b');
    draw();
    CHECK_LINE(0, "line 1");
    CHECK(strstr(status(), "lines 1-11 ") != NULL);
    key(KEY_PGDN);
    key('j');
    key('\r');
    key(KEY_DOWN);
    draw();
    CHECK_LINE(0, "line 15");
    key('k');
    key(KEY_UP);
    draw();
    CHECK_LINE(0, "line 13");
    key('d');                               // half a page: 5 lines
    draw();
    CHECK_LINE(0, "line 18");
    key('u');
    draw();
    CHECK_LINE(0, "line 13");
    key('G');                               // the last line on the bottom row
    draw();
    CHECK_LINE(0, "line 90");
    CHECK_LINE(10, "line 100");
    CHECK(strstr(status(), "lines 90-100/100 END") != NULL);
    key(' ');                               // can't go further
    draw();
    CHECK_LINE(0, "line 90");
    key('k');
    draw();
    CHECK_LINE(0, "line 89");
    CHECK(strstr(status(), "lines 89-99/100 ") != NULL);
    CHECK(strstr(status(), "END") == NULL);
    key('g');
    draw();
    CHECK_LINE(0, "line 1");
    CHECK(strstr(status(), "lines 1-11/100 ") != NULL);
    key(KEY_END);
    draw();
    CHECK_LINE(10, "line 100");
    key(KEY_HOME);
    draw();
    CHECK_LINE(0, "line 1");
    pager_free(&pg);
    free(text);
}

TEST(a_small_window_reads_the_text_in_pieces) {
    char *text = numbered(300, " of the text");            // about 5.5 KB
    open_text(text, 10, 40, 64);                           // 64 bytes of it at a time
    CHECK_EQ(pg.win_cap, 64);
    draw();
    CHECK_LINE(0, "line 1 of the text");
    CHECK_LINE(8, "line 9 of the text");
    CHECK(source.largest <= 64);
    for (int i = 0; i < 20; i++)
        key(' ');
    draw();
    CHECK_LINE(0, "line 181 of the text");
    CHECK(strstr(status(), "lines 181-189") != NULL);
    key('G');
    draw();
    CHECK_LINE(8, "line 300 of the text");
    CHECK(strstr(status(), "lines 292-300/300 END") != NULL);
    for (int i = 0; i < 7; i++)
        key('k');
    draw();
    CHECK_LINE(0, "line 285 of the text");
    key('g');
    keys("/line 250 \r");                   // across many windows
    draw();
    CHECK_LINE(0, "line 250 of the text");
    CHECK(strstr(status(), "found on line 250") != NULL);
    CHECK(source.reads > 50);               // it really went through the window
    CHECK(source.largest <= 64);
    pager_free(&pg);
    free(text);
}

TEST(the_window_is_loaded_only_at_its_edge) {
    char *text = numbered(300, " of the text");            // about 5.5 KB
    open_text(text, 10, 40, 1024);
    draw();
    int reads = source.reads;
    for (int i = 0; i < 40; i++)            // to the end, 9 lines a time
        key(' ');
    draw();
    CHECK(strstr(status(), "END") != NULL);
    // Forward through 5.5 KB in 1 KB windows, plus the walk back from the end: a handful of
    // reads, not one per line.
    CHECK(source.reads - reads < 20);
    reads = source.reads;
    for (int i = 0; i < 40; i++)
        key('b');
    draw();
    CHECK_LINE(0, "line 1 of the text");
    CHECK(source.reads - reads < 20);
    pager_free(&pg);
    free(text);
}

TEST(a_match_across_the_window_edge) {
    static char text[200];
    memset(text, '0', 60);
    strcpy(text + 60, "needle\nmore\nlast\n");           // "needle" at 60..65 crosses 64
    open_text(text, 6, 80, 64);
    keys("/needle\r");                     // on the top line, through the edge
    draw();
    CHECK_EQ(pg.top, 0);
    CHECK(strstr(status(), "found on line 1") != NULL);
    CHECK_EQ(pg.hit, 60);
    pager_free(&pg);

    memset(text, '0', 62);
    strcpy(text + 62, "\nmore\nlast\n");                 // a match starting with the '\n' at 62
    open_text(text, 6, 80, 64);
    strcpy(pg.last_find, "\nmore");          // (a '\n' can't be typed at the prompt)
    key('n');
    draw();
    CHECK_EQ(pg.hit, 62);
    CHECK_EQ(pg.top, 0);                    // the line holding the match's start
    CHECK_EQ(pg.top_line, 0);
    CHECK(strstr(status(), "found on line 1") != NULL);
    key('n');                               // past it: none, around to the same one
    draw();
    CHECK_EQ(pg.hit, 62);
    CHECK(strstr(status(), "search wrapped around: found on line 1") != NULL);
    pager_free(&pg);
}

TEST(a_taller_terminal_pulls_the_view_back_to_the_end) {
    char *text = numbered(30, "");
    open_text(text, 6, 40, 0);
    key('G');
    CHECK_EQ(pg.top_line, 25);
    pager_resize(&pg, 20, 40);              // 19 text rows: the last screenful starts at line 12
    t_reset(20, 40);
    draw();
    CHECK_LINE(0, "line 12");
    CHECK_LINE(18, "line 30");
    CHECK(strstr(status(), "lines 12-30/30 END") != NULL);
    pager_free(&pg);
    free(text);
}

TEST(search_forward_wraps_and_fails) {
    char *text = numbered(60, "");
    open_text(text, 6, 40, 0);                 // 5 text rows
    keys("/line 4\r");
    draw();
    CHECK_LINE(0, "line 4");
    CHECK(strstr(status(), "found on line 4") != NULL);
    key('j');                               // any key: the status row is back
    key('k');
    draw();
    CHECK(strstr(status(), "lines 4-8") != NULL);
    key('n');
    draw();
    CHECK_LINE(0, "line 40");
    key('n');
    draw();
    CHECK_LINE(0, "line 41");
    for (int i = 0; i < 8; i++)
        key('n');
    draw();
    CHECK_LINE(0, "line 49");
    key('n');                               // none after: around to the first
    draw();
    CHECK_LINE(0, "line 4");
    CHECK(strstr(status(), "search wrapped around") != NULL);
    keys("/line 59\r");                     // in the last screenful: the end is shown
    draw();
    CHECK_LINE(0, "line 56");
    CHECK_LINE(3, "line 59");
    CHECK(strstr(status(), "found on line 59") != NULL);
    key('x');                               // a key that does nothing clears the message
    draw();
    CHECK(strstr(status(), "lines 56-60/60 END") != NULL);
    keys("/nothing here\r");
    draw();
    CHECK(strstr(status(), "not found") != NULL);
    CHECK_LINE(0, "line 56");               // didn't move
    keys("\x06" "abc");                     // Ctrl-F opens the prompt too
    draw();
    CHECK(strstr(status(), "Search: abc") != NULL);
    CHECK(t_cursor_on);
    CHECK_EQ(t_col, 11);
    keys("\x7f\x7f\x7f\x7f");               // backspace past the start
    key('\r');                              // empty: the last search again
    draw();
    CHECK(strstr(status(), "not found") != NULL);
    keys("/x");
    key(0x03);
    draw();
    CHECK(!pg.finding);
    pager_free(&pg);
    free(text);
}

TEST(a_match_on_a_page_shorter_than_the_screen_is_shown) {
    open_text("NAME\n    cat - show a file\nDESCRIPTION\n    a secure secure boot\n    more\n", 10, 40, 0);
    keys("/secure\r");
    draw();
    CHECK_EQ(pg.top, 0);                    // nothing to scroll
    CHECK(strstr(status(), "found on line 4") != NULL);
    CHECK_LINE(3, "    a secure secure boot");
    CHECK_REV(3, "      ^^^^^^ ^^^^^^");     // every match on screen
    CHECK_REV(1, "");
    CHECK(!t_rev_on);                       // switched off at the end of each one
    key('n');                               // the next one, on the same line
    draw();
    CHECK_EQ(pg.hit, (size_t)(strstr(source.text, "secure secure") - source.text) + 7);
    CHECK(strstr(status(), "found on line 4") != NULL);
    key('n');                               // none after: around to the first
    draw();
    CHECK(strstr(status(), "search wrapped around: found on line 4") != NULL);
    keys("/e\r");                           // no capital: NAME's E matches too
    draw();
    CHECK_REV(0, "   ^");
    CHECK_REV(1, "                    ^");    // "show a file": the 'e'
    pager_free(&pg);

    open_text("aaaa b aa\n", 5, 40, 0);     // overlapping matches: one span
    keys("/aa\r");
    draw();
    CHECK_REV(0, "^^^^   ^^");
    pager_free(&pg);
}

TEST(a_new_search_starts_on_the_top_line) {
    char *text = numbered(30, "");
    open_text(text, 6, 40, 0);
    keys("/line 1\r");
    draw();
    CHECK_LINE(0, "line 1");
    CHECK_EQ(pg.hit, 0);
    CHECK(strstr(status(), "found on line 1") != NULL);
    CHECK_REV(0, "^^^^^^");
    key('n');                               // then past it
    draw();
    CHECK_LINE(0, "line 10");
    key('j');
    key('j');
    keys("/line 1\r");                      // a new search: from the top line shown, line 12
    draw();
    CHECK_LINE(0, "line 12");
    pager_free(&pg);
    free(text);
}

TEST(n_goes_through_the_last_screenful_and_wraps) {
    char *text = numbered(30, "");
    open_text(text, 6, 40, 0);              // 5 text rows: the last screenful is lines 26-30
    keys("/line 2\r");
    draw();
    CHECK_LINE(0, "line 2");
    for (int i = 20; i <= 26; i++) {
        key('n');
        draw();
        char want[16];
        snprintf(want, sizeof(want), "line %d", i);
        CHECK_LINE(0, want);
    }
    // The view is on the last screenful: n still goes on, line by line, without moving it.
    for (int i = 27; i <= 29; i++) {
        key('n');
        draw();
        char want[32];
        snprintf(want, sizeof(want), "found on line %d", i);
        CHECK(strstr(status(), want) != NULL);
        CHECK_LINE(0, "line 26");
        CHECK_REV(i - 26, "^^^^^^");
    }
    key('n');                               // none after line 29: around to line 2
    draw();
    CHECK_LINE(0, "line 2");
    CHECK(strstr(status(), "search wrapped around: found on line 2") != NULL);
    key('G');                               // scrolled away from the match: n starts at the top
    key('k');
    key('k');                               // top: line 24
    key('n');
    draw();
    CHECK_LINE(0, "line 24");
    CHECK(strstr(status(), "found on line 24") != NULL);
    pager_free(&pg);
    free(text);
}

TEST(a_search_without_capitals_ignores_case) {
    open_text("Secure boot\nSECURE\nsecure\n", 6, 40, 0);
    keys("/secure\r");
    draw();
    CHECK(strstr(status(), "found on line 1") != NULL);
    CHECK_REV(0, "^^^^^^");
    CHECK_REV(1, "^^^^^^");
    CHECK_REV(2, "^^^^^^");
    keys("/SECURE\r");                      // a capital: exact
    draw();
    CHECK(strstr(status(), "found on line 2") != NULL);
    CHECK_REV(0, "");
    CHECK_REV(1, "^^^^^^");
    CHECK_REV(2, "");
    keys("/Boot\r");
    draw();
    CHECK(strstr(status(), "not found") != NULL);
    pager_free(&pg);
}

TEST(a_match_cut_by_an_edge_leaves_nothing_inverted) {
    open_text("0123456789abcdefghijklmnopqrstuvwxyz\nshort\n", 4, 20, 0);
    keys("/hijklm\r");                      // cut by the right edge
    draw();
    CHECK_REV(0, "                 ^^^");
    CHECK(!t_rev_on);
    key(KEY_RIGHT);                         // and by the left one: columns 10.. shown
    draw();
    CHECK_LINE(0, "abcdefghijklmnopqrst");
    CHECK_REV(0, "       ^^^^^^");
    CHECK(!t_rev_on);
    key(KEY_RIGHT);                         // only its end is shown: columns 20..
    keys("/fghijk\r");
    draw();
    CHECK_LINE(0, "klmnopqrstuvwxyz");
    CHECK_REV(0, "^");
    CHECK(!t_rev_on);
    pager_free(&pg);
}

TEST(matches_in_wide_and_marked_text) {
    // "日本" is two wide characters; "é" a letter with a combining mark (one column).
    open_text("ab 日本 x e\xcc\x81" "e\xcc\x81 z\n", 4, 20, 0);
    keys("/日本\r");
    draw();
    CHECK_REV(0, "   ^^^^");
    keys("/e\r");                           // matches the letter: the unit is shown inverted
    draw();
    CHECK_REV(0, "          ^^");
    keys("/\xcc\x81" "e\r");                // starts on a mark, inside a unit
    draw();
    CHECK(strstr(status(), "found on line 1") != NULL);
    CHECK_REV(0, "          ^^");
    CHECK(!t_rev_on);
    pager_free(&pg);
}

TEST(long_lines_scroll_sideways) {
    open_text("0123456789abcdefghijklmnopqrstuvwxyz\nshort\n", 4, 20, 0);
    key('j');
    draw();
    CHECK_LINE(0, "0123456789abcdefghij");
    CHECK_LINE(1, "short");
    key(KEY_RIGHT);                         // half a screen
    draw();
    CHECK_LINE(0, "abcdefghijklmnopqrst");
    CHECK_LINE(1, "");
    key(KEY_RIGHT);
    draw();
    CHECK_LINE(0, "klmnopqrstuvwxyz");
    key(KEY_LEFT);
    key(KEY_LEFT);
    key(KEY_LEFT);
    draw();
    CHECK_LINE(0, "0123456789abcdefghij");
    pager_free(&pg);
}

TEST(controls_and_wide_characters) {
    open_text("a\tb\x01" "c\n\xe6\xbc\xa2\xe5\xad\x97 wide\n", 4, 20, 0);
    key('j');
    draw();
    CHECK_LINE(0, "a       b?c");
    CHECK_LINE(1, "\xe6\xbc\xa2\xe5\xad\x97 wide");
    pager_free(&pg);
}

TEST(quit_resize_and_log_messages) {
    open_text("x\n", 5, 80, 0);
    key('h');
    draw();
    CHECK(strstr(status(), "q quit") != NULL);     // the whole key list fits 80 columns
    CHECK_EQ(key('q'), PAGER_QUIT);
    CHECK_EQ(key('Q'), PAGER_QUIT);
    CHECK_EQ(key(0x11), PAGER_QUIT);        // Ctrl-Q
    CHECK_EQ(key(0x03), PAGER_QUIT);        // Ctrl-C
    CHECK_EQ(key(0x0c), PAGER_RESIZE);      // Ctrl-L
    CHECK_EQ(key('j'), PAGER_NONE);
    pager_set_log_waiting(&pg, 2);
    draw();
    CHECK(strstr(status(), "2 log messages held (dmesg)") != NULL);
    pager_resize(&pg, 1000, 1000);
    CHECK_EQ(pg.scr.rows, SCREEN_ROWS_MAX);
    CHECK_EQ(pg.scr.cols, SCREEN_COLS_MAX);
    pager_free(&pg);
}

TEST(a_read_error_is_reported) {
    char *text = numbered(200, " of the text");
    open_text(text, 8, 40, 128);
    source.fail_from = 1000;
    key('G');                               // counting the lines reads past the failure
    draw();
    CHECK(strstr(status(), "read error") != NULL);
    CHECK(pg.failed);
    key('g');                               // the message stays, and no total was kept
    draw();
    CHECK(strstr(status(), "read error") != NULL);
    CHECK_EQ(pg.lines, -1);
    pager_free(&pg);
    free(text);
}

TEST(frames_in_small_pieces_show_the_same) {
    char *text = numbered(30, " of the text, a bit longer than the screen is wide");
    open_text(text, 8, 30, 0);
    static char out[24];
    int calls = 0;
    while (!screen_drawn(&pg.scr) && calls < 10000) {
        t_feed(out, screen_draw(&pg.scr, out, sizeof(out)));
        calls++;
    }
    CHECK(screen_drawn(&pg.scr));
    CHECK_LINE(0, "line 1 of the text, a bit long");
    CHECK_LINE(6, "line 7 of the text, a bit long");
    key(' ');
    while (!screen_drawn(&pg.scr))
        t_feed(out, screen_draw(&pg.scr, out, sizeof(out)));
    CHECK_LINE(0, "line 8 of the text, a bit long");
    CHECK(strstr(status(), "lines 8-14") != NULL);
    pager_free(&pg);
    free(text);
}

TEST(a_big_terminal_is_used_whole) {
    char *text = numbered(500, "");
    open_text(text, 150, 300, 0);
    key('j');
    draw();
    CHECK_LINE(0, "line 2");
    CHECK_LINE(148, "line 150");
    CHECK(strstr(status(), "lines 2-150") != NULL);
    key('G');
    draw();
    CHECK_LINE(148, "line 500");
    CHECK(strstr(status(), "lines 352-500/500 END") != NULL);
    pager_free(&pg);
    free(text);
}

TEST(random_keys_keep_invariants) {
    srand(4321);
    char *text = numbered(400, " words to find");
    open_text(text, 9, 30, 100);
    static const int pool[] = {
        ' ', 'b', 'j', 'k', 'd', 'u', 'g', 'G', 'n', '/', 'l', 'i', '\r', 0x7f, 'h',
        KEY_UP, KEY_DOWN, KEY_PGUP, KEY_PGDN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END,
    };
    static char out[300];
    for (int i = 0; i < 5000; i++) {
        key(pool[rand() % (int)(sizeof(pool) / sizeof(pool[0]))]);
        CHECK(pg.top <= pg.src.size);
        CHECK(pg.top_line >= 0);
        if (i % 5 == 0)
            while (!screen_drawn(&pg.scr))
                t_feed(out, screen_draw(&pg.scr, out, sizeof(out)));
    }
    CHECK(!pg.failed);
    pager_free(&pg);
    free(text);
}

// --- a text that grows while it is shown ---------------------------------------------------------

static char *grow_buf;                      // the growing text, re-pointed as it moves
static size_t grow_len, grow_cap;

static void grow_open(int rows, int cols, size_t window) {
    grow_len = 0;
    grow_cap = 16;
    grow_buf = malloc(grow_cap);
    pager_source_t src = { .read = pager_read_mem, .ctx = grow_buf, .size = 0, .growing = true };
    CHECK(pager_init(&pg, "grow.txt", src, window));
    t_reset(rows, cols);
    pager_resize(&pg, rows, cols);
}

// Append text the way a caller does: realloc, re-point, tell the pager.
static void grow_add(const char *s) {
    size_t n = strlen(s);
    if (grow_len + n > grow_cap) {
        while (grow_len + n > grow_cap)
            grow_cap *= 2;
        grow_buf = realloc(grow_buf, grow_cap);     // may move: the pager is told below
        pg.src.ctx = grow_buf;
    }
    memcpy(grow_buf + grow_len, s, n);
    grow_len += n;
    pager_grow(&pg, grow_len);
}

static void grow_close(void) {
    pager_free(&pg);
    free(grow_buf);
    grow_buf = NULL;
}

TEST(a_growing_source_gets_the_whole_window) {
    grow_open(6, 40, 0);
    CHECK_EQ(pg.win_cap, PAGER_WINDOW);
    key('j');                               // the hint gives way to the status
    draw();
    CHECK_LINE(0, "~");
    CHECK(strstr(status(), "empty") != NULL);
    grow_add("first\n");
    draw();
    CHECK_LINE(0, "first");
    CHECK_LINE(1, "~");
    grow_close();
    open_text("abc\n", 6, 40, 0);           // a fixed source keeps the small window
    CHECK_EQ(pg.win_cap, 4);
    pager_free(&pg);
}

TEST(growth_while_the_view_is_at_the_end) {
    grow_open(6, 40, 0);                    // 5 text rows
    grow_add("one\ntwo\n");
    key('j');
    draw();
    CHECK_LINE(1, "two");
    CHECK_LINE(2, "~");
    CHECK(strstr(status(), "lines 1-2/... END") != NULL);   // no total while growing
    grow_add("three\nfour\nfive\nsix\nseven\n");   // the realloc moves the block more than once
    draw();
    CHECK_LINE(0, "one");                   // the view didn't move: more is below it now
    CHECK_LINE(4, "five");
    CHECK(strstr(status(), "END") == NULL);
    key('G');                               // the end so far
    draw();
    CHECK_LINE(0, "three");
    CHECK_LINE(4, "seven");
    CHECK(strstr(status(), "lines 3-7/... END") != NULL);
    grow_add("eight\n");                    // rows past the end were showing: the frame changes
    CHECK(!screen_drawn(&pg.scr));
    draw();
    CHECK_LINE(4, "seven");
    key(' ');
    key('G');
    draw();
    CHECK_LINE(4, "eight");
    grow_add("nine");                       // a last line without its '\n'
    key('G');
    draw();
    CHECK_LINE(4, "nine");
    grow_add(" and more\n");                // the same line, longer
    draw();
    CHECK_LINE(4, "nine and more");
    grow_close();
}

TEST(growth_while_scrolled_back_does_not_move_the_view) {
    grow_open(6, 40, 0);
    for (int i = 1; i <= 20; i++) {
        char l[32];
        snprintf(l, sizeof(l), "line %d\n", i);
        grow_add(l);
    }
    key('G');
    key('b');                               // a page back from the end so far
    draw();
    CHECK_LINE(0, "line 11");
    char before[64];
    snprintf(before, sizeof(before), "%s", status());
    size_t top = pg.top;
    long top_line = pg.top_line;
    grow_add("line 21\nline 22\nline 23\n");
    CHECK(!screen_drawn(&pg.scr));          // the percentage changed
    draw();
    CHECK_LINE(0, "line 11");
    CHECK_EQ(pg.top, top);
    CHECK_EQ(pg.top_line, top_line);
    CHECK(strcmp(before, status()) != 0);
    CHECK(strstr(status(), "lines 11-15/...") != NULL);
    key('G');
    draw();
    CHECK_LINE(4, "line 23");
    grow_close();
}

// A fresh count of the growing text: '\n's, plus one for a last line without one.
static long fresh_count(void) {
    long n = 0;
    for (size_t i = 0; i < grow_len; i++)
        n += grow_buf[i] == '\n';
    return grow_len > 0 && grow_buf[grow_len - 1] != '\n' ? n + 1 : n;
}

TEST(the_line_count_after_growth_is_a_fresh_count) {
    const char *pieces[] = { "a\nb\n", "c", "d\ne", "\n", "\n\nf\n", "g", "h", "\n" };
    grow_open(6, 40, 64);
    for (size_t i = 0; i < sizeof(pieces) / sizeof(pieces[0]); i++) {
        key('G');
        draw();
        CHECK_EQ(pg.lines, fresh_count());  // counted, as the end was on screen
        grow_add(pieces[i]);
        CHECK_EQ(pg.lines, fresh_count());  // extended, not forgotten
    }
    pager_set_growing(&pg, false);
    key('G');
    draw();
    CHECK(strstr(status(), "/8 END") != NULL);
    grow_close();
}

TEST(the_status_row_while_growing) {
    grow_open(6, 70, 0);
    grow_add("x\ny\n");
    key('h');
    draw();
    CHECK(strstr(status(), "F follow") != NULL);
    key('j');
    draw();
    CHECK(strstr(status(), "lines 1-2/... END") != NULL);
    pager_set_growing(&pg, false);          // complete: the total shows
    draw();
    CHECK(strstr(status(), "lines 1-2/2 END") != NULL);
    CHECK(strstr(status(), "...") == NULL);
    grow_close();
}

TEST(follow_stays_on_the_end_until_a_movement_key) {
    grow_open(5, 40, 0);                    // 4 text rows
    grow_add("1\n2\n3\n4\n5\n6\n");
    key('F');
    draw();
    CHECK_LINE(0, "3");
    CHECK(strstr(status(), "FOLLOW") != NULL);
    grow_add("7\n8\n");
    draw();
    CHECK_LINE(0, "5");
    CHECK_LINE(3, "8");
    key('h');                               // not a movement key
    CHECK(pg.follow);
    key('k');
    CHECK(!pg.follow);
    grow_add("9\n");
    draw();
    CHECK_LINE(0, "4");                     // stayed where it was left
    CHECK(strstr(status(), "FOLLOW") == NULL);
    key('F');
    grow_add("10\n");
    draw();
    CHECK_LINE(3, "10");
    pager_set_growing(&pg, false);
    CHECK(!pg.follow);
    grow_close();
}

TEST(random_keys_with_growth_keep_invariants) {
    srand(8765);
    grow_open(9, 30, 100);
    static const int pool[] = {
        ' ', 'b', 'j', 'k', 'd', 'u', 'g', 'G', 'n', '/', 'l', 'i', '\r', 0x7f, 'h', 'F',
        KEY_UP, KEY_DOWN, KEY_PGUP, KEY_PGDN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END,
    };
    static char out[300];
    int lines = 0;
    for (int i = 0; i < 5000; i++) {
        if (rand() % 4 == 0) {
            char l[64];
            snprintf(l, sizeof(l), rand() % 7 ? "line %d words to find\n" : "line %d no end", lines++);
            grow_add(l);
        } else {
            key(pool[rand() % (int)(sizeof(pool) / sizeof(pool[0]))]);
        }
        CHECK(pg.top <= pg.src.size);
        CHECK(pg.top_line >= 0);
        if (i % 5 == 0)
            while (!screen_drawn(&pg.scr))
                t_feed(out, screen_draw(&pg.scr, out, sizeof(out)));
    }
    CHECK(!pg.failed);
    grow_close();
}

TEST_MAIN(
    RUN(shows_text_and_status);
    RUN(no_newline_at_the_end_and_empty_text);
    RUN(pages_lines_and_ends);
    RUN(a_small_window_reads_the_text_in_pieces);
    RUN(the_window_is_loaded_only_at_its_edge);
    RUN(a_match_across_the_window_edge);
    RUN(a_taller_terminal_pulls_the_view_back_to_the_end);
    RUN(search_forward_wraps_and_fails);
    RUN(a_match_on_a_page_shorter_than_the_screen_is_shown);
    RUN(a_new_search_starts_on_the_top_line);
    RUN(n_goes_through_the_last_screenful_and_wraps);
    RUN(a_search_without_capitals_ignores_case);
    RUN(a_match_cut_by_an_edge_leaves_nothing_inverted);
    RUN(matches_in_wide_and_marked_text);
    RUN(long_lines_scroll_sideways);
    RUN(controls_and_wide_characters);
    RUN(quit_resize_and_log_messages);
    RUN(a_read_error_is_reported);
    RUN(frames_in_small_pieces_show_the_same);
    RUN(a_big_terminal_is_used_whole);
    RUN(random_keys_keep_invariants);
    RUN(a_growing_source_gets_the_whole_window);
    RUN(growth_while_the_view_is_at_the_end);
    RUN(growth_while_scrolled_back_does_not_move_the_view);
    RUN(the_line_count_after_growth_is_a_fresh_count);
    RUN(the_status_row_while_growing);
    RUN(follow_stays_on_the_end_until_a_movement_key);
    RUN(random_keys_with_growth_keep_invariants);
)
