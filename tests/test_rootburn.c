// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/rootburn.c: writing the root secret into OTP, against a model of
// OTP in which a bit, once set, stays set. A row written with error correction keeps its 16
// data bits in bits 15:0 and a stand-in for the correction bits in bits 21:16 (never zero, so
// that a written row of zeros isn't blank, as on the chip).
//
// The property under test, beyond "it works": whatever goes wrong and wherever, the board is
// never left with a page that reads as a root secret unless the whole write verified, and
// never with a spent page that the bootloader could still write.

#include <string.h>

#include "core/rootburn.h"
#include "test.h"

#define ROWS 4096
static uint32_t otp[ROWS];
static uint32_t lock1[64];
static int writes;                      // programs of any kind, counted
static int fail_write_at = -1;          // the nth program from now reports failure, writes nothing
static int silent_write_at = -1;        // the nth program from now reports success, writes nothing
static int cut_at = -1;                 // "power cut": the nth program lands, every later one doesn't
static bool rng_fails, rng_zero;
static int present_calls, present_until = -1;   // present() says no from this call on
static unsigned rng_seed;

static uint32_t ecc_bits(uint16_t v) { return ((uint32_t)(v * 37u + 11u) & 0x1fu) << 16 | 0x200000u; }

static bool gate(void) {                // may this program change anything, and what does it report
    writes++;
    return true;
}
static int mode(void) {                 // 0 land, 1 report failure, 2 silently nothing
    int m = 0;
    if (fail_write_at == 0)
        m = 1;
    else if (silent_write_at == 0)
        m = 2;
    else if (cut_at == 0)
        m = 2;                          // after a cut nothing lands (cut_at stays 0)
    if (fail_write_at >= 0 && m != 1)
        fail_write_at--;
    else if (m == 1)
        fail_write_at = -1;
    if (silent_write_at > 0)
        silent_write_at--;
    else if (silent_write_at == 0)
        silent_write_at = -1;
    if (cut_at > 0)
        cut_at--;
    return m;
}

static uint32_t f_raw(void *c, unsigned row) { (void)c; return otp[row]; }
static uint16_t f_ecc(void *c, unsigned row) { (void)c; return (uint16_t)otp[row]; }
static bool f_careful(void *c, unsigned row, unsigned n, uint16_t *out) {
    (void)c;
    for (unsigned i = 0; i < n; i++) {
        uint32_t v = otp[row + i];
        // The guarded read refuses a row whose correction bits don't fit its data (on the
        // chip: a fault). A blank row reads as zero.
        if (v != 0 && (v & 0x3f0000u) != ecc_bits((uint16_t)v))
            return false;
        out[i] = (uint16_t)v;
    }
    return true;
}
static uint32_t f_lock1(void *c, unsigned page) { (void)c; return lock1[page]; }

static bool f_write_ecc(void *c, unsigned row, uint16_t value) {
    (void)c;
    gate();
    int m = mode();
    if (m == 1)
        return false;
    if (m == 0)
        otp[row] |= ecc_bits(value) | value;
    return true;
}
static bool f_write_raw(void *c, unsigned row, uint32_t bits) {
    (void)c;
    gate();
    int m = mode();
    if (m == 1)
        return false;
    if (m == 0)
        otp[row] |= bits & 0xffffffu;
    return true;
}
static bool f_write_lock1(void *c, unsigned page, uint32_t bits) {
    (void)c;
    gate();
    int m = mode();
    if (m == 1)
        return false;
    if (m == 0)
        lock1[page] |= bits & 0xffffffu;
    return true;
}
static bool f_random(void *c, uint8_t *out, size_t len) {
    (void)c;
    if (rng_fails)
        return false;
    for (size_t i = 0; i < len; i++) {
        rng_seed = rng_seed * 1103515245u + 12345u;
        out[i] = rng_zero ? 0 : (uint8_t)(rng_seed >> 16);
    }
    return true;
}
static bool f_present(void *c) {
    (void)c;
    return present_until < 0 || present_calls++ < present_until;
}

static const rootburn_ops_t ops = {
    NULL, { NULL, f_raw, f_ecc, f_careful, f_lock1 },
    f_write_ecc, f_write_raw, f_write_lock1, f_random, f_present,
};
static const roototp_reader_t *rd = &ops.reader;

static void fresh(void) {
    memset(otp, 0, sizeof(otp));
    memset(lock1, 0, sizeof(lock1));
    writes = 0;
    fail_write_at = silent_write_at = cut_at = -1;
    rng_fails = rng_zero = false;
    present_calls = 0;
    present_until = -1;
    rng_seed = 12345;
}

#define P(i) (ROOTOTP_FIRST_PAGE + (i))
static bool locked(unsigned index) { return !roototp_bootloader_can_write(lock1[P(index)]); }
static bool page_blank(unsigned index) { return roototp_page(rd, index) == ROOTOTP_BLANK; }

// The invariant: no page reads as a usable secret unless `expect_secret`, and no page that
// holds anything is left writable from the bootloader.
static void check_invariant(bool expect_secret) {
    unsigned index;
    uint8_t s[ROOTOTP_SECRET_LEN];
    roototp_find_t f = roototp_find(rd, &index);
    if (expect_secret) {
        CHECK_EQ(f, ROOTOTP_FOUND);
        CHECK(roototp_read(rd, index, s));
        CHECK(roototp_locks_ok(rd, index, NULL));
    } else {
        CHECK(f != ROOTOTP_FOUND || !roototp_read(rd, index, s));
    }
    for (unsigned i = 0; i < ROOTOTP_PAGES; i++)
        if (!page_blank(i))
            CHECK(locked(i));
    for (unsigned row = 0; row < ROWS; row++)           // nothing outside the list's pages
        if (row < ROOTOTP_ROW(0, 0) || row > ROOTOTP_ROW(ROOTOTP_PAGES - 1, 63))
            CHECK_EQ(otp[row], 0);
}

TEST(a_clean_burn) {
    rootburn_report_t rep;
    uint8_t s[ROOTOTP_SECRET_LEN];
    unsigned index;
    fresh();
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_OK);
    CHECK_EQ(rep.page, P(0));
    CHECK(rep.wrote && rep.page_locked);
    CHECK_EQ(roototp_find(rd, &index), ROOTOTP_FOUND);
    CHECK_EQ(index, 0);
    CHECK(roototp_read(rd, 0, s));
    CHECK_EQ(lock1[P(0)], ROOTBURN_LOCK_WORD);
    // The chaff is the complement of the secret's raw rows.
    for (unsigned k = 0; k < ROOTOTP_SECRET_ROWS; k++)
        CHECK_EQ(otp[ROOTOTP_ROW(0, ROOTOTP_CHAFF_ROW + k)], ~otp[ROOTOTP_ROW(0, k)] & 0xffffffu);
    // Rows the format leaves blank are blank.
    for (unsigned row = 19; row < ROOTOTP_CHAFF_ROW; row++)
        CHECK_EQ(otp[ROOTOTP_ROW(0, row)], 0);
    for (unsigned row = ROOTOTP_CHAFF_ROW + 16; row < 64; row++)
        CHECK_EQ(otp[ROOTOTP_ROW(0, row)], 0);
    CHECK(page_blank(1) && page_blank(2));
    CHECK_EQ(lock1[P(1)], 0);
    check_invariant(true);
    // 16 secret rows, 16 chaff, 2 check, the marker, the lock.
    CHECK_EQ(writes, 16 + 16 + 2 + 1 + 1);
}

// A second run on a board that has its secret writes nothing.
TEST(nothing_is_written_twice) {
    rootburn_report_t rep;
    uint32_t before[ROWS];
    fresh();
    rootburn_run(&ops, false, &rep);
    memcpy(before, otp, sizeof(otp));
    int w = writes;
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_HAS_SECRET);
    CHECK(!rep.wrote);
    CHECK_EQ(writes, w);
    CHECK(!memcmp(before, otp, sizeof(otp)));
    rootburn_run(&ops, true, &rep);                     // nor does a rehearsal
    CHECK_EQ(rep.result, ROOTBURN_HAS_SECRET);
    CHECK_EQ(writes, w);
}

// The rehearsal, as planned for dev-1: stop on purpose, then the real run uses the next page.
TEST(rehearsal_then_the_real_run) {
    rootburn_report_t rep;
    uint8_t first[ROOTOTP_SECRET_LEN], real[ROOTOTP_SECRET_LEN];
    unsigned index;
    fresh();
    rootburn_run(&ops, true, &rep);
    CHECK_EQ(rep.result, ROOTBURN_REHEARSED);
    CHECK_EQ(rep.page, P(0));
    CHECK(rep.page_locked);                             // not left for anyone to finish
    CHECK_EQ(roototp_page(rd, 0), ROOTOTP_SPENT);
    CHECK_EQ(roototp_find(rd, &index), ROOTOTP_NONE_YET);
    CHECK_EQ(index, 1);
    check_invariant(false);
    for (unsigned k = 0; k < ROOTOTP_SECRET_ROWS; k++) {
        first[2 * k] = (uint8_t)otp[ROOTOTP_ROW(0, k)];
        first[2 * k + 1] = (uint8_t)(otp[ROOTOTP_ROW(0, k)] >> 8);
    }
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_OK);
    CHECK_EQ(rep.page, P(1));
    CHECK_EQ(roototp_find(rd, &index), ROOTOTP_FOUND);
    CHECK_EQ(index, 1);
    CHECK(roototp_read(rd, 1, real));
    CHECK(memcmp(real, first, sizeof(real)) != 0);      // fresh bytes, not the spent page's
    check_invariant(true);
}

// A spent page left unlocked by an earlier run that died (power lost before it could lock):
// the next run locks it before anything else.
TEST(an_unlocked_spent_page_is_locked_first) {
    rootburn_report_t rep;
    fresh();
    for (unsigned k = 0; k < 5; k++)                    // a few rows, then nothing
        otp[ROOTOTP_ROW(0, k)] = ecc_bits(0x1111) | 0x1111;
    CHECK(!locked(0));
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_OK);
    CHECK_EQ(rep.page, P(1));
    CHECK(locked(0));
    check_invariant(true);
    // If that lock can't be written, no secret is written at all.
    fresh();
    otp[ROOTOTP_ROW(0, 3)] = ecc_bits(7) | 7;
    fail_write_at = 0;
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_EARLIER_LOCK);
    CHECK_EQ(rep.page, P(0));
    CHECK(page_blank(1));
    fresh();
    otp[ROOTOTP_ROW(0, 3)] = ecc_bits(7) | 7;
    silent_write_at = 0;                                // it "succeeds" and nothing changes
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_EARLIER_LOCK);
    CHECK(page_blank(1));
}

TEST(nobody_there_means_nothing_written) {
    rootburn_report_t rep;
    fresh();
    present_until = 0;                                  // never present
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_NOT_PRESENT);
    CHECK(!rep.wrote);
    CHECK_EQ(writes, 0);
    fresh();
    present_until = 1;                                  // let go before the first write
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_NOT_PRESENT);
    CHECK_EQ(writes, 0);
    CHECK(page_blank(0));
}

TEST(no_random_bytes_means_nothing_written) {
    rootburn_report_t rep;
    fresh();
    rng_fails = true;
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_RANDOM);
    CHECK_EQ(writes, 0);
    CHECK(page_blank(0));
}

TEST(no_usable_page) {
    rootburn_report_t rep;
    fresh();
    for (unsigned i = 0; i < ROOTOTP_PAGES; i++)        // all spent
        otp[ROOTOTP_ROW(i, 0)] = ecc_bits(1) | 1;
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_NO_PAGE);
    CHECK_EQ(writes, 0);
    fresh();                                            // a damaged marker: not to be built on
    otp[ROOTOTP_ROW(0, ROOTOTP_MARKER_ROW)] = ecc_bits(0x1234) | 0x1234;
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_NO_PAGE);
    CHECK_EQ(writes, 0);
    CHECK(page_blank(1));
}

// Every single program, in turn, reports failure. The run must stop there, never leave a
// usable secret behind, and leave the page it spent locked (unless the lock itself is what
// failed, or nothing had been written yet).
TEST(a_failed_write_anywhere) {
    const int total = 16 + 16 + 2 + 1 + 1;
    for (int n = 0; n < total; n++) {
        rootburn_report_t rep;
        fresh();
        fail_write_at = n;
        rootburn_run(&ops, false, &rep);
        bool lock_step = n == total - 1;
        if (lock_step) {
            // Everything verified and the marker is in, but the lock isn't: said loudly, and
            // the secret is there (the firmware will warn, or refuse on a sealed-only board).
            CHECK_EQ(rep.result, ROOTBURN_LOCK);
            CHECK(!rep.page_locked);
        } else {
            CHECK_EQ(rep.result, ROOTBURN_WRITE);
            CHECK_EQ(rep.page, P(0));
            unsigned index;
            uint8_t s[ROOTOTP_SECRET_LEN];
            CHECK(roototp_find(rd, &index) != ROOTOTP_FOUND || !roototp_read(rd, index, s));
            if (n > 0) {
                CHECK(rep.page_locked);
                check_invariant(false);
            } else {
                // The very first row's write failed and wrote nothing: the page is still
                // blank; locking it anyway costs a page and is what happens.
                CHECK(rep.page_locked);
            }
        }
    }
}

// The same for a program that reports success and changes nothing (the read-back catches it).
TEST(a_write_that_did_not_take_anywhere) {
    const int total = 16 + 16 + 2 + 1 + 1;
    for (int n = 0; n < total; n++) {
        rootburn_report_t rep;
        unsigned index;
        uint8_t s[ROOTOTP_SECRET_LEN];
        fresh();
        silent_write_at = n;
        rootburn_run(&ops, false, &rep);
        if (n == total - 1) {
            CHECK_EQ(rep.result, ROOTBURN_LOCK);
        } else {
            CHECK(rep.result == ROOTBURN_WRITE || rep.result == ROOTBURN_VERIFY);
            CHECK(roototp_find(rd, &index) != ROOTOTP_FOUND || !roototp_read(rd, index, s));
            CHECK(rep.page_locked);
        }
    }
}

// Power lost after the nth program: that one lands, nothing after it does, including the
// lock. Whatever n is, the board is not left with a usable secret unless the marker landed,
// and the next run (power back) repairs the locks and gives the board its secret.
TEST(power_lost_anywhere_then_run_again) {
    const int total = 16 + 16 + 2 + 1 + 1;
    for (int n = 0; n < total; n++) {
        rootburn_report_t rep;
        unsigned index;
        uint8_t s[ROOTOTP_SECRET_LEN];
        fresh();
        cut_at = n;                                     // programs 0..n-1 land
        rootburn_run(&ops, false, &rep);
        roototp_find_t f = roototp_find(rd, &index);
        bool marker_landed = n >= total - 1;            // all but the lock landed
        if (marker_landed) {
            CHECK_EQ(f, ROOTOTP_FOUND);                 // complete and verified, lock missing
            CHECK(roototp_read(rd, index, s));
            CHECK(!roototp_locks_ok(rd, index, NULL));  // which the firmware's check reports
        } else {
            CHECK(f != ROOTOTP_FOUND);
        }
        // Power is back: run again.
        cut_at = -1;
        rootburn_run(&ops, false, &rep);
        if (marker_landed) {
            CHECK_EQ(rep.result, ROOTBURN_HAS_SECRET);  // it won't write a second one
        } else {
            CHECK_EQ(rep.result, ROOTBURN_OK);
            CHECK_EQ(rep.page, n == 0 ? P(0) : P(1));   // n == 0: nothing had landed at all
            check_invariant(true);
        }
    }
}

// Rows that already hold a stray bit before the burn (the chip repairs one per row; this
// model doesn't, so the row doesn't read back): the page is spent and the next one is used.
TEST(a_page_that_will_not_take_the_secret) {
    rootburn_report_t rep;
    fresh();
    otp[ROOTOTP_ROW(0, ROOTOTP_CHAFF_ROW + 2)] = 0;     // (blank page, but:)
    otp[ROOTOTP_ROW(1, 9)] = 0;
    // Page 0 has a bit stuck in a chaff row: not blank, so it is spent and skipped.
    otp[ROOTOTP_ROW(0, ROOTOTP_CHAFF_ROW + 2)] = 0x000400;
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_OK);
    CHECK_EQ(rep.page, P(1));
    CHECK(locked(0));
    check_invariant(true);
}

// A random number generator that returns zeros is not our business to second-guess here, but
// the write must still be whole: a secret of zeros is 16 written rows, not a blank page.
TEST(a_secret_of_zeros_is_still_a_written_page) {
    rootburn_report_t rep;
    uint8_t s[ROOTOTP_SECRET_LEN];
    fresh();
    rng_zero = true;
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_OK);
    CHECK(roototp_read(rd, 0, s));
    CHECK(!page_blank(0));
}

TEST(result_texts) {
    for (int r = ROOTBURN_OK; r <= ROOTBURN_LOCK; r++)
        CHECK(strlen(rootburn_result_text((rootburn_result_t)r)) > 10);
    CHECK_EQ(ROOTBURN_LOCK_WORD, 0x3d3d3d);
    CHECK(!roototp_bootloader_can_write(ROOTBURN_LOCK_WORD));
}

TEST_MAIN(
    RUN(a_clean_burn);
    RUN(nothing_is_written_twice);
    RUN(rehearsal_then_the_real_run);
    RUN(an_unlocked_spent_page_is_locked_first);
    RUN(nobody_there_means_nothing_written);
    RUN(no_random_bytes_means_nothing_written);
    RUN(no_usable_page);
    RUN(a_failed_write_anywhere);
    RUN(a_write_that_did_not_take_anywhere);
    RUN(power_lost_anywhere_then_run_again);
    RUN(a_page_that_will_not_take_the_secret);
    RUN(a_secret_of_zeros_is_still_a_written_page);
    RUN(result_texts);
)
