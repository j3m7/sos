// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/roototp.c: finding the root secret in OTP pages, and choosing where
// a board takes its root secret from. The OTP here is an array of rows; a row's low 16 bits
// are its data and bits 16..21 stand for its error-correction bits (set whenever the row was
// written, so that a written row of zeros isn't blank, as on the chip).

#include <string.h>

#include "core/roototp.h"
#include "test.h"

#define ROWS 4096
static uint32_t otp[ROWS];
static bool unreadable_page[64];
static bool careful_fails;              // the careful read reports it can't
static bool careful_zeros;              // the careful read is "glitched" into returning zeros
static int careful_calls;

static uint32_t f_raw(void *ctx, unsigned row) {
    (void)ctx;
    return unreadable_page[row / 64] ? 0xffffffffu : otp[row];
}
static uint16_t f_ecc(void *ctx, unsigned row) {
    (void)ctx;
    return unreadable_page[row / 64] ? 0xffff : (uint16_t)otp[row];
}
static bool f_careful(void *ctx, unsigned row, unsigned count, uint16_t *out) {
    (void)ctx;
    careful_calls++;
    if (careful_fails)
        return false;
    for (unsigned i = 0; i < count; i++)
        out[i] = careful_zeros ? 0 : (uint16_t)otp[row + i];
    return true;
}
// Page lock words (PAGEn_LOCK1), as raw rows: the lock byte three times over.
static uint32_t lock1[64];
static uint32_t f_lock1(void *ctx, unsigned page) { (void)ctx; return lock1[page]; }
#define LOCK(byte) ((uint32_t)(byte) | (uint32_t)(byte) << 8 | (uint32_t)(byte) << 16)
#define BL_READ_WRITE 0x00
#define BL_READ_ONLY  0x10
#define BL_NO_ACCESS  0x30
#define AS_BURNED     0x3d              // bootloader none, Non-secure none, Secure read-only
static const roototp_reader_t reader = { NULL, f_raw, f_ecc, f_careful, f_lock1 };

static void write_row(unsigned row, uint16_t value) { otp[row] = 0x150000u | value; }

static void fresh(void) {
    memset(otp, 0, sizeof(otp));
    memset(unreadable_page, 0, sizeof(unreadable_page));
    memset(lock1, 0, sizeof(lock1));
    careful_fails = careful_zeros = false;
    careful_calls = 0;
}

static void secret_for(unsigned n, uint8_t s[ROOTOTP_SECRET_LEN]) {
    for (unsigned i = 0; i < ROOTOTP_SECRET_LEN; i++)
        s[i] = (uint8_t)(n * 31 + i * 7 + 1);
}

// What the burn image writes, in its order, stopping after `steps` of: 1 the secret rows,
// 2 the chaff, 3 the check, 4 the marker.
static void burn(unsigned index, const uint8_t s[ROOTOTP_SECRET_LEN], int steps) {
    uint8_t check[ROOTOTP_CHECK_LEN];
    if (steps >= 1)
        for (unsigned k = 0; k < ROOTOTP_SECRET_ROWS; k++)
            write_row(ROOTOTP_ROW(index, k), (uint16_t)(s[2 * k] | s[2 * k + 1] << 8));
    if (steps >= 2)
        for (unsigned k = 0; k < ROOTOTP_SECRET_ROWS; k++)
            otp[ROOTOTP_ROW(index, ROOTOTP_CHAFF_ROW + k)] = ~otp[ROOTOTP_ROW(index, k)] & 0xffffffu;
    if (steps >= 3) {
        CHECK(roototp_check(s, check));
        for (unsigned k = 0; k < ROOTOTP_CHECK_ROWS; k++)
            write_row(ROOTOTP_ROW(index, ROOTOTP_CHECK_ROW + k),
                      (uint16_t)(check[2 * k] | check[2 * k + 1] << 8));
    }
    if (steps >= 4)
        write_row(ROOTOTP_ROW(index, ROOTOTP_MARKER_ROW), ROOTOTP_MARKER);
}

TEST(layout) {
    CHECK_EQ(ROOTOTP_ROW(0, 0), 0x100);         // page 4
    CHECK_EQ(ROOTOTP_ROW(1, 0), 0x140);
    CHECK_EQ(ROOTOTP_ROW(2, 63), 0x1bf);        // the last row of page 6
    CHECK(ROOTOTP_FIRST_PAGE > 3);              // page 3 is the console-only marker's
    CHECK(ROOTOTP_MARKER_ROW >= ROOTOTP_CHECK_ROW + ROOTOTP_CHECK_ROWS);
    CHECK(ROOTOTP_CHAFF_ROW > ROOTOTP_MARKER_ROW);
    CHECK(ROOTOTP_CHAFF_ROW + ROOTOTP_SECRET_ROWS <= ROOTOTP_PAGE_ROWS);
}

TEST(a_blank_board_has_none_yet) {
    unsigned index = 99;
    fresh();
    CHECK_EQ(roototp_page(&reader, 0), ROOTOTP_BLANK);
    CHECK_EQ(roototp_find(&reader, &index), ROOTOTP_NONE_YET);
    CHECK_EQ(index, 0);
    CHECK_EQ(careful_calls, 0);                 // finding never reads the secret
}

TEST(a_burned_secret_is_found_and_read) {
    uint8_t s[ROOTOTP_SECRET_LEN], got[ROOTOTP_SECRET_LEN];
    unsigned index = 99;
    fresh();
    secret_for(1, s);
    burn(0, s, 4);
    CHECK_EQ(roototp_page(&reader, 0), ROOTOTP_COMPLETE);
    CHECK_EQ(roototp_find(&reader, &index), ROOTOTP_FOUND);
    CHECK_EQ(index, 0);
    CHECK(roototp_read(&reader, index, got));
    CHECK(!memcmp(got, s, sizeof(s)));
}

// A burn that stopped anywhere before the marker leaves a spent page: skipped, and the next
// page decides. This is the rehearsal's interrupted burn.
TEST(an_interrupted_burn_is_spent_and_skipped) {
    uint8_t a[ROOTOTP_SECRET_LEN], b[ROOTOTP_SECRET_LEN], got[ROOTOTP_SECRET_LEN];
    unsigned index;
    secret_for(1, a);
    secret_for(2, b);
    for (int steps = 1; steps <= 3; steps++) {
        fresh();
        burn(0, a, steps);
        CHECK_EQ(roototp_page(&reader, 0), ROOTOTP_SPENT);
        CHECK_EQ(roototp_find(&reader, &index), ROOTOTP_NONE_YET);
        CHECK_EQ(index, 1);                     // the next page is the one to use
        burn(1, b, 4);
        CHECK_EQ(roototp_find(&reader, &index), ROOTOTP_FOUND);
        CHECK_EQ(index, 1);
        CHECK(roototp_read(&reader, index, got));
        CHECK(!memcmp(got, b, sizeof(b)));      // the second page's secret, not the spent one
    }
    fresh();                                    // a single row, even one bit, spends a page
    otp[ROOTOTP_ROW(0, 40)] = 1;
    CHECK_EQ(roototp_page(&reader, 0), ROOTOTP_SPENT);
}

TEST(the_first_complete_page_wins) {
    uint8_t a[ROOTOTP_SECRET_LEN], b[ROOTOTP_SECRET_LEN], got[ROOTOTP_SECRET_LEN];
    unsigned index;
    fresh();
    secret_for(1, a);
    secret_for(2, b);
    burn(0, a, 4);
    burn(1, b, 4);                              // shouldn't happen; if it does, page order rules
    CHECK_EQ(roototp_find(&reader, &index), ROOTOTP_FOUND);
    CHECK_EQ(index, 0);
    CHECK(roototp_read(&reader, index, got));
    CHECK(!memcmp(got, a, sizeof(a)));
}

TEST(all_pages_spent) {
    uint8_t a[ROOTOTP_SECRET_LEN];
    unsigned index;
    fresh();
    secret_for(1, a);
    for (unsigned i = 0; i < ROOTOTP_PAGES; i++)
        burn(i, a, 2);
    CHECK_EQ(roototp_find(&reader, &index), ROOTOTP_EXHAUSTED);
    burn(ROOTOTP_PAGES, a, 4);                  // a page beyond the list is not looked at
    CHECK_EQ(roototp_find(&reader, &index), ROOTOTP_EXHAUSTED);
}

// A page that can't be read is never "blank" and never skipped: it stops the search.
TEST(unreadable_pages_stop_the_search) {
    uint8_t a[ROOTOTP_SECRET_LEN];
    unsigned index;
    fresh();
    secret_for(1, a);
    unreadable_page[ROOTOTP_FIRST_PAGE] = true;
    CHECK_EQ(roototp_page(&reader, 0), ROOTOTP_UNREADABLE);
    CHECK_EQ(roototp_find(&reader, &index), ROOTOTP_CANT_READ);
    burn(1, a, 4);                              // a secret further on is not reached
    CHECK_EQ(roototp_find(&reader, &index), ROOTOTP_CANT_READ);
    fresh();
    burn(0, a, 2);                              // spent, then unreadable
    unreadable_page[ROOTOTP_FIRST_PAGE + 1] = true;
    CHECK_EQ(roototp_find(&reader, &index), ROOTOTP_CANT_READ);
    CHECK_EQ(index, 1);
    fresh();                                    // after the page was locked for this boot
    burn(0, a, 4);
    unreadable_page[ROOTOTP_FIRST_PAGE] = true;
    CHECK_EQ(roototp_find(&reader, &index), ROOTOTP_CANT_READ);
}

// The check catches a secret that doesn't read as it was written: one bit off anywhere, a
// wrong check, or a read that returns zeros.
TEST(a_secret_that_fails_its_check_is_not_used) {
    uint8_t s[ROOTOTP_SECRET_LEN], got[ROOTOTP_SECRET_LEN];
    unsigned index;
    secret_for(3, s);
    for (unsigned row = 0; row < ROOTOTP_SECRET_ROWS + ROOTOTP_CHECK_ROWS; row++) {
        for (unsigned bit = 0; bit < 16; bit += 5) {
            fresh();
            burn(0, s, 4);
            otp[ROOTOTP_ROW(0, row)] ^= 1u << bit;
            CHECK_EQ(roototp_find(&reader, &index), ROOTOTP_FOUND);     // the marker is there
            memset(got, 0xaa, sizeof(got));
            CHECK(!roototp_read(&reader, index, got));
            for (unsigned i = 0; i < sizeof(got); i++)
                CHECK_EQ(got[i], 0);            // nothing of it is handed out
        }
    }
    fresh();
    burn(0, s, 4);
    careful_zeros = true;
    CHECK(!roototp_read(&reader, 0, got));
    careful_zeros = false;
    careful_fails = true;
    memset(got, 0xaa, sizeof(got));
    CHECK(!roototp_read(&reader, 0, got));
    CHECK_EQ(got[0], 0);
    careful_fails = false;
    CHECK(roototp_read(&reader, 0, got));
    CHECK(!roototp_read(&reader, ROOTOTP_PAGES, got));  // not a page of the list
}

TEST(the_check_depends_on_every_byte) {
    uint8_t s[ROOTOTP_SECRET_LEN], c0[ROOTOTP_CHECK_LEN], c[ROOTOTP_CHECK_LEN];
    secret_for(4, s);
    CHECK(roototp_check(s, c0));
    for (unsigned i = 0; i < ROOTOTP_SECRET_LEN; i++) {
        s[i] ^= 0x10;
        CHECK(roototp_check(s, c));
        CHECK(memcmp(c, c0, sizeof(c)) != 0);
        s[i] ^= 0x10;
    }
    CHECK(roototp_check(s, c));
    CHECK(!memcmp(c, c0, sizeof(c)));
}

// Where the root secret comes from. The rule that matters most: on a console-only board it is
// OTP or nothing, whatever is in OTP and whatever is in flash.
TEST(source) {
    // A secret in OTP, with its pages locked, is used, console-only or not.
    CHECK_EQ(roototp_choose(ROOTOTP_FOUND, true, true, false), ROOTSRC_OTP);
    CHECK_EQ(roototp_choose(ROOTOTP_FOUND, true, true, true), ROOTSRC_OTP);
    // With a list page the bootloader can still write: an open board uses it (and warns); a
    // console-only board does not.
    CHECK_EQ(roototp_choose(ROOTOTP_FOUND, true, false, false), ROOTSRC_OTP);
    CHECK_EQ(roototp_choose(ROOTOTP_FOUND, true, false, true), ROOTSRC_NONE);
    // A secret in OTP that can't be used is replaced by nothing.
    for (int locks = 0; locks <= 1; locks++) {
        CHECK_EQ(roototp_choose(ROOTOTP_FOUND, false, locks, false), ROOTSRC_NONE);
        CHECK_EQ(roototp_choose(ROOTOTP_FOUND, false, locks, true), ROOTSRC_NONE);
    }
    // An open board with nothing in OTP uses its flash record, as before.
    CHECK_EQ(roototp_choose(ROOTOTP_NONE_YET, false, false, false), ROOTSRC_FLASH);
    CHECK_EQ(roototp_choose(ROOTOTP_EXHAUSTED, false, false, false), ROOTSRC_FLASH);
    // ... but not if what is there can't be read or made sense of.
    CHECK_EQ(roototp_choose(ROOTOTP_CANT_READ, false, false, false), ROOTSRC_NONE);
    CHECK_EQ(roototp_choose(ROOTOTP_BAD_MARKER, false, false, false), ROOTSRC_NONE);
    // A console-only board never uses the flash record, whatever else is true.
    for (int found = ROOTOTP_FOUND; found <= ROOTOTP_BAD_MARKER; found++)
        for (int ok = 0; ok <= 1; ok++)
            for (int locks = 0; locks <= 1; locks++) {
                rootsrc_t src = roototp_choose((roototp_find_t)found, ok, locks, true);
                CHECK(src != ROOTSRC_FLASH);
                if (found != ROOTOTP_FOUND)
                    CHECK_EQ(src, ROOTSRC_NONE);
            }
}

// The marker row decides between spent and complete, and anything else stops the search: a
// row that isn't blank and doesn't read as the marker must not be passed over as "spent".
TEST(a_damaged_marker_stops_the_search) {
    uint8_t a[ROOTOTP_SECRET_LEN], b[ROOTOTP_SECRET_LEN];
    unsigned index;
    fresh();
    secret_for(1, a);
    secret_for(2, b);
    burn(0, a, 3);
    write_row(ROOTOTP_ROW(0, ROOTOTP_MARKER_ROW), ROOTOTP_MARKER ^ 0x0100);    // not the marker
    CHECK_EQ(roototp_page(&reader, 0), ROOTOTP_DAMAGED);
    CHECK_EQ(roototp_find(&reader, &index), ROOTOTP_BAD_MARKER);
    CHECK_EQ(index, 0);
    burn(1, b, 4);                              // a complete page further on is not reached
    CHECK_EQ(roototp_find(&reader, &index), ROOTOTP_BAD_MARKER);
    fresh();
    burn(0, a, 3);
    otp[ROOTOTP_ROW(0, ROOTOTP_MARKER_ROW)] = 0x400000;     // one stray bit, data bits zero
    CHECK_EQ(roototp_page(&reader, 0), ROOTOTP_DAMAGED);
    fresh();                                    // the marker row alone, on an otherwise blank page
    write_row(ROOTOTP_ROW(0, ROOTOTP_MARKER_ROW), 0x1234);
    CHECK_EQ(roototp_find(&reader, &index), ROOTOTP_BAD_MARKER);
}

// The lock word: LOCK_BL is bits 5:4 of a byte stored three times, each bit by two of three.
TEST(lock_word) {
    CHECK(roototp_bootloader_can_write(LOCK(BL_READ_WRITE)));       // a blank lock word
    CHECK(!roototp_bootloader_can_write(LOCK(BL_READ_ONLY)));
    CHECK(!roototp_bootloader_can_write(LOCK(BL_NO_ACCESS)));
    CHECK(!roototp_bootloader_can_write(LOCK(AS_BURNED)));
    CHECK(!roototp_bootloader_can_write(LOCK(0x20)));               // the reserved value: locked
    CHECK(roototp_bootloader_can_write(LOCK(0x0f)));                // only the other fields set
    CHECK(roototp_bootloader_can_write(0xffffffffu));               // can't be read: not trusted
    // One copy out of three doesn't decide, either way.
    CHECK(roototp_bootloader_can_write(0x000010));                  // a write that stopped early
    CHECK(roototp_bootloader_can_write(0x100000));
    CHECK(!roototp_bootloader_can_write(0x001010));
    CHECK(!roototp_bootloader_can_write(0x3d3d00));
    CHECK(!roototp_bootloader_can_write(0x3d003d));
}

// Every page of the list up to the secret's must be locked against the bootloader's writes,
// the spent ones included: an unlocked spent page can be completed by someone with a cable.
TEST(pages_up_to_the_secret_must_be_locked) {
    unsigned open_page = 0;
    fresh();
    for (unsigned i = 0; i < ROOTOTP_PAGES; i++) {
        CHECK(!roototp_locks_ok(&reader, i, &open_page));           // nothing locked
        CHECK_EQ(open_page, ROOTOTP_FIRST_PAGE);
    }
    lock1[ROOTOTP_FIRST_PAGE] = LOCK(AS_BURNED);
    CHECK(roototp_locks_ok(&reader, 0, &open_page));                // the secret in the first page
    CHECK(!roototp_locks_ok(&reader, 1, &open_page));               // in the second: it isn't locked
    CHECK_EQ(open_page, ROOTOTP_FIRST_PAGE + 1);
    lock1[ROOTOTP_FIRST_PAGE + 1] = LOCK(AS_BURNED);
    CHECK(roototp_locks_ok(&reader, 1, NULL));
    // dev-1 as planned: the first page spent, the secret in the second. The spent page
    // unlocked is exactly the case to catch.
    lock1[ROOTOTP_FIRST_PAGE] = LOCK(BL_READ_WRITE);
    CHECK(!roototp_locks_ok(&reader, 1, &open_page));
    CHECK_EQ(open_page, ROOTOTP_FIRST_PAGE);
    lock1[ROOTOTP_FIRST_PAGE] = LOCK(BL_READ_ONLY);                 // read-only is enough
    CHECK(roototp_locks_ok(&reader, 1, NULL));
    // Pages after the secret's don't matter: the search never reaches them.
    CHECK_EQ(lock1[ROOTOTP_FIRST_PAGE + 2], 0);
    CHECK(roototp_locks_ok(&reader, 1, NULL));
    CHECK(!roototp_locks_ok(&reader, 2, &open_page));
    CHECK_EQ(open_page, ROOTOTP_FIRST_PAGE + 2);
    lock1[ROOTOTP_FIRST_PAGE + 1] = 0xffffffffu;                    // a lock word that can't be read
    CHECK(!roototp_locks_ok(&reader, 1, &open_page));
    CHECK_EQ(open_page, ROOTOTP_FIRST_PAGE + 1);
}

TEST_MAIN(
    RUN(layout);
    RUN(a_blank_board_has_none_yet);
    RUN(a_burned_secret_is_found_and_read);
    RUN(an_interrupted_burn_is_spent_and_skipped);
    RUN(the_first_complete_page_wins);
    RUN(all_pages_spent);
    RUN(unreadable_pages_stop_the_search);
    RUN(a_secret_that_fails_its_check_is_not_used);
    RUN(the_check_depends_on_every_byte);
    RUN(source);
    RUN(a_damaged_marker_stops_the_search);
    RUN(lock_word);
    RUN(pages_up_to_the_secret_must_be_locked);
)
