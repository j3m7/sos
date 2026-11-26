// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/rootburn.c: writing the root secret into OTP, against a model of
// OTP that behaves as the chip and its boot ROM are documented to:
//
//   - a bit, once set, stays set;
//   - a write fails if it would leave a set bit unset (the ROM refuses it), so a raw row can
//     only be written to a value that includes what it holds, and an error-corrected row only
//     when blank (or to what it already holds);
//   - a page whose lock word says Secure code can't write refuses every write, the lock word
//     included (it is "write-protected by its own permissions").
//
// A row written with error correction keeps its 16 data bits in bits 15:0 and a stand-in for
// the correction bits above them (never zero, so that a written row of zeros isn't blank).
//
// The properties under test, beyond "it works": whatever goes wrong and wherever, the board
// is never left with a page that reads as a root secret unless the whole write verified;
// never with a written page the bootloader could still write, once a run has finished; and
// never with a blank page that is locked, which would block the list for good.

#include <string.h>

#include "core/rootburn.h"
#include "test.h"

#define ROWS 4096
static uint32_t otp[ROWS];
static uint32_t lock1[64];
static int writes, ticks;
static int fail_write_at = -1;          // the nth program from now reports failure, writes nothing
static int silent_write_at = -1;        // the nth program from now reports success, writes nothing
static int cut_at = -1;                 // power lost: n more programs land, none after
static bool rng_fails, rng_same;
static int present_calls, present_until = -1;   // present() says no from this call on
static unsigned rng_seed;

static uint32_t ecc_bits(uint16_t v) { return ((uint32_t)(v * 37u + 11u) & 0x1fu) << 16 | 0x200000u; }
static bool sec_locked(unsigned page) { return !roototp_secure_can_write(lock1[page]); }

// 0: the program lands; 1: it reports failure; 2: it reports success and nothing changes.
static int mode(void) {
    int m = 0;
    writes++;
    if (cut_at == 0)
        return 2;                       // power is gone: nothing lands any more
    if (cut_at > 0)
        cut_at--;
    if (fail_write_at == 0)
        m = 1;
    if (silent_write_at == 0)
        m = 2;
    if (fail_write_at >= 0)
        fail_write_at--;
    if (silent_write_at >= 0)
        silent_write_at--;
    return m;
}

static uint32_t f_raw(void *c, unsigned row) { (void)c; return otp[row]; }
static uint16_t f_ecc(void *c, unsigned row) { (void)c; return (uint16_t)otp[row]; }
static bool f_careful(void *c, unsigned row, unsigned n, uint16_t *out) {
    (void)c;
    for (unsigned i = 0; i < n; i++) {
        uint32_t v = otp[row + i];
        // A row whose correction bits don't fit its data doesn't read (on the chip: a fault).
        if (v != 0 && (v & 0x3f0000u) != ecc_bits((uint16_t)v))
            return false;
        out[i] = (uint16_t)v;
    }
    return true;
}
static uint32_t f_lock1(void *c, unsigned page) { (void)c; return lock1[page]; }

static bool f_write_ecc(void *c, unsigned row, uint16_t value) {
    (void)c;
    int m = mode();
    if (m == 1)
        return false;
    if (m == 2)
        return true;
    uint32_t want = ecc_bits(value) | value;
    if (sec_locked(row / 64) || (otp[row] & ~want))
        return false;                   // locked, or it would have to clear a bit
    otp[row] |= want;
    return true;
}
static bool f_write_raw(void *c, unsigned row, uint32_t bits) {
    (void)c;
    int m = mode();
    if (m == 1)
        return false;
    if (m == 2)
        return true;
    if (sec_locked(row / 64) || (otp[row] & ~bits))
        return false;
    otp[row] |= bits & 0xffffffu;
    return true;
}
static bool f_write_lock1(void *c, unsigned page, uint32_t bits) {
    (void)c;
    int m = mode();
    if (m == 1)
        return false;
    if (m == 2)
        return true;
    if (sec_locked(page) || (lock1[page] & ~bits))
        return false;
    lock1[page] |= bits & 0xffffffu;
    return true;
}
static bool f_random(void *c, uint8_t *out, size_t len) {
    (void)c;
    if (rng_fails)
        return false;
    for (size_t i = 0; i < len; i++) {
        rng_seed = rng_seed * 1103515245u + 12345u;
        out[i] = rng_same ? 0x5a : (uint8_t)(rng_seed >> 16);
    }
    return true;
}
static bool f_present(void *c) {
    (void)c;
    return present_until < 0 || present_calls++ < present_until;
}
static void f_tick(void *c) { (void)c; ticks++; }

static const rootburn_ops_t ops = {
    NULL, { NULL, f_raw, f_ecc, f_careful, f_lock1 },
    f_write_ecc, f_write_raw, f_write_lock1, f_tick, f_random, f_present,
};
static const roototp_reader_t *rd = &ops.reader;

static void no_faults(void) {
    fail_write_at = silent_write_at = cut_at = -1;
    rng_fails = rng_same = false;
    present_calls = 0;
    present_until = -1;
}
static void fresh(void) {
    memset(otp, 0, sizeof(otp));
    memset(lock1, 0, sizeof(lock1));
    writes = ticks = 0;
    no_faults();
    rng_seed = 12345;
}

#define P(i) (ROOTOTP_FIRST_PAGE + (i))
#define PROGRAMS (16 + 16 + 2 + 1 + 1)  // a clean burn: secret, chaff, check, marker, lock
static bool locked(unsigned index) { return !roototp_bootloader_can_write(lock1[P(index)]); }
static bool rows_blank(unsigned index) {
    for (unsigned row = 0; row < 64; row++)
        if (otp[ROOTOTP_ROW(index, row)])
            return false;
    return true;
}

// What must hold whenever a run has returned and the board could be walked away from.
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
    for (unsigned i = 0; i < ROOTOTP_PAGES; i++) {
        if (rows_blank(i))
            CHECK_EQ(lock1[P(i)], 0);   // a blank page is never locked
        else
            CHECK(locked(i));           // a written page is never left to the bootloader
    }
    for (unsigned row = 0; row < ROWS; row++)           // nothing outside the list's pages
        if (row < ROOTOTP_ROW(0, 0) || row > ROOTOTP_ROW(ROOTOTP_PAGES - 1, 63))
            CHECK_EQ(otp[row], 0);
    for (unsigned page = 0; page < 64; page++)
        if (page < P(0) || page > P(ROOTOTP_PAGES - 1))
            CHECK_EQ(lock1[page], 0);
}

// The board gets its secret, on the expected page, from a run with nothing going wrong.
static void run_clean(unsigned expect_page) {
    rootburn_report_t rep;
    no_faults();
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_OK);
    CHECK_EQ(rep.page, expect_page);
    check_invariant(true);
}

TEST(a_clean_burn) {
    rootburn_report_t rep;
    uint8_t s[ROOTOTP_SECRET_LEN];
    unsigned index;
    fresh();
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_OK);
    CHECK_EQ(rep.page, P(0));
    CHECK(rep.wrote && rep.page_locked && !rep.nothing_landed);
    CHECK_EQ(roototp_find(rd, &index), ROOTOTP_FOUND);
    CHECK_EQ(index, 0);
    CHECK(roototp_read(rd, 0, s));
    CHECK_EQ(lock1[P(0)], ROOTBURN_LOCK_WORD);
    for (unsigned k = 0; k < ROOTOTP_SECRET_ROWS; k++)  // chaff = complement of the raw rows
        CHECK_EQ(otp[ROOTOTP_ROW(0, ROOTOTP_CHAFF_ROW + k)], ~otp[ROOTOTP_ROW(0, k)] & 0xffffffu);
    for (unsigned row = 19; row < ROOTOTP_CHAFF_ROW; row++)
        CHECK_EQ(otp[ROOTOTP_ROW(0, row)], 0);          // rows the format leaves blank
    for (unsigned row = ROOTOTP_CHAFF_ROW + 16; row < 64; row++)
        CHECK_EQ(otp[ROOTOTP_ROW(0, row)], 0);
    CHECK(rows_blank(1) && rows_blank(2));
    check_invariant(true);
    CHECK_EQ(writes, PROGRAMS);
    CHECK(ticks >= PROGRAMS);                           // the watchdog is fed before every write
}

// A second run on a board that has its secret, locked, writes nothing.
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
    run_clean(P(1));
    CHECK(roototp_read(rd, 1, real));
    CHECK(memcmp(real, first, sizeof(real)) != 0);      // fresh bytes, not the spent page's
}

// THE CASE THE FIRST VERSION GOT WRONG. The very first write fails and nothing lands (the boot
// ROM call being wrong for error-corrected rows, say). The page is still blank. It must not
// be locked: a locked blank page can't be written, and the search for a secret never moves
// past a blank page, so the whole list would be blocked for good. It is left as it was, and
// the next run uses the same page.
TEST(a_first_write_that_fails_leaves_the_page_blank_and_unlocked) {
    rootburn_report_t rep;
    for (int silent = 0; silent <= 1; silent++) {
        fresh();
        if (silent)
            silent_write_at = 0;
        else
            fail_write_at = 0;
        rootburn_run(&ops, false, &rep);
        CHECK_EQ(rep.result, ROOTBURN_WRITE);
        CHECK(rep.nothing_landed);
        CHECK(!rep.page_locked && !rep.wrote);
        CHECK(rows_blank(0));
        CHECK_EQ(lock1[P(0)], 0);
        CHECK(rootburn_must_stop(rep.result));          // and it is still a reason to stop
        check_invariant(false);
        run_clean(P(0));                                // the same page, not the next
    }
    // The same in a rehearsal.
    fresh();
    fail_write_at = 0;
    rootburn_run(&ops, true, &rep);
    CHECK_EQ(rep.result, ROOTBURN_WRITE);
    CHECK(rep.nothing_landed);
    CHECK_EQ(lock1[P(0)], 0);
    // Every write failing, as a wrong ROM call would: run after run, no page is ever locked
    // or spent.
    fresh();
    for (int run = 0; run < 5; run++) {
        fail_write_at = 0;
        rootburn_run(&ops, false, &rep);
        CHECK(rep.nothing_landed);
    }
    check_invariant(false);
    for (unsigned i = 0; i < ROOTOTP_PAGES; i++)
        CHECK(rows_blank(i));
    run_clean(P(0));
}

// A blank page that is locked against the firmware all the same (not made by this code; by a
// stray picotool command, say) can never hold a secret. Reader and burn pass over it, so that
// a lock alone can't block the list.
TEST(a_locked_blank_page_is_passed_over) {
    rootburn_report_t rep;
    unsigned index;
    fresh();
    lock1[P(0)] = 0x010101;                             // Secure read-only, nothing written
    CHECK_EQ(roototp_page(rd, 0), ROOTOTP_SPENT);
    CHECK_EQ(roototp_find(rd, &index), ROOTOTP_NONE_YET);
    CHECK_EQ(index, 1);
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_OK);
    CHECK_EQ(rep.page, P(1));
    CHECK(rows_blank(0));
    CHECK_EQ(lock1[P(0)], 0x010101);                    // left alone: nothing to protect in it
    CHECK_EQ(roototp_find(rd, &index), ROOTOTP_FOUND);
    CHECK_EQ(index, 1);
    // Locked against the bootloader only: the firmware can still write it, so it is used.
    fresh();
    lock1[P(0)] = 0x101010;
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_OK);
    CHECK_EQ(rep.page, P(0));
}

// A spent page left unlocked by an earlier run that died (power lost before it could lock):
// the next run locks it before anything else, and writes no secret if it can't.
TEST(an_unlocked_spent_page_is_locked_first) {
    rootburn_report_t rep;
    fresh();
    for (unsigned k = 0; k < 5; k++)
        otp[ROOTOTP_ROW(0, k)] = ecc_bits(0x1111) | 0x1111;
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_OK);
    CHECK_EQ(rep.page, P(1));
    CHECK(locked(0));
    check_invariant(true);
    for (int silent = 0; silent <= 1; silent++) {
        fresh();
        otp[ROOTOTP_ROW(0, 3)] = ecc_bits(7) | 7;
        if (silent)
            silent_write_at = 0;
        else
            fail_write_at = 0;
        rootburn_run(&ops, false, &rep);
        CHECK_EQ(rep.result, ROOTBURN_EARLIER_LOCK);
        CHECK_EQ(rep.page, P(0));
        CHECK(rows_blank(1));
        CHECK(rootburn_must_stop(rep.result));
    }
}

// The boot ROM refuses a write that would leave a set bit unset. A lock word that already
// holds a bit outside the pattern is locked by writing what it holds plus the pattern.
TEST(a_lock_word_with_bits_already_set) {
    rootburn_report_t rep;
    fresh();
    lock1[P(0)] = 0x400000;                             // a stray bit, no lock in effect
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_OK);
    CHECK_EQ(lock1[P(0)], 0x400000 | ROOTBURN_LOCK_WORD);
    CHECK(locked(0));
    fresh();
    lock1[P(0)] = 0x000030;                             // one copy of a bootloader lock: no effect yet
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_OK);
    CHECK(locked(0));
}

TEST(nobody_there_means_nothing_written) {
    rootburn_report_t rep;
    fresh();
    present_until = 0;
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_NOT_PRESENT);
    CHECK(!rep.wrote);
    CHECK_EQ(writes, 0);
    fresh();
    present_until = 1;                                  // let go before the first write
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_NOT_PRESENT);
    CHECK_EQ(writes, 0);
    CHECK(rows_blank(0));
    CHECK(!rootburn_must_stop(rep.result));             // just try again
}

TEST(bad_random_bytes_mean_nothing_written) {
    rootburn_report_t rep;
    fresh();
    rng_fails = true;
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_RANDOM);
    CHECK_EQ(writes, 0);
    fresh();
    rng_same = true;                                    // 32 equal bytes: a broken generator
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_RANDOM);
    CHECK_EQ(writes, 0);
    CHECK(rows_blank(0));
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
    CHECK(rows_blank(1));
}

// Every single program, in turn, reports failure (`silent` 0) or reports success and changes
// nothing (`silent` 1). After the run, and again after a second, clean run, the invariant
// holds; and the second run always ends with the board having its secret.
TEST(a_bad_write_anywhere_then_run_again) {
    for (int silent = 0; silent <= 1; silent++) {
        for (int n = 0; n < PROGRAMS; n++) {
            rootburn_report_t rep;
            fresh();
            if (silent)
                silent_write_at = n;
            else
                fail_write_at = n;
            rootburn_run(&ops, false, &rep);
            if (n == 0) {
                CHECK(rep.result == ROOTBURN_WRITE && rep.nothing_landed);
                check_invariant(false);
                run_clean(P(0));
            } else if (n == PROGRAMS - 1) {
                // Everything verified and the marker is in; the lock didn't take. The secret
                // is there and unlocked: said loudly, and the next run locks it.
                CHECK_EQ(rep.result, ROOTBURN_LOCK);
                CHECK(!rep.page_locked);
                no_faults();
                rootburn_run(&ops, false, &rep);
                CHECK_EQ(rep.result, ROOTBURN_RELOCKED);
                CHECK(rep.page_locked);
                check_invariant(true);
            } else {
                CHECK(rep.result == ROOTBURN_WRITE || rep.result == ROOTBURN_VERIFY);
                CHECK(!rep.nothing_landed);
                CHECK(rep.page_locked);
                CHECK(rootburn_must_stop(rep.result));
                check_invariant(false);
                run_clean(P(1));
            }
        }
    }
}

// Two things going wrong at once: a write fails partway and then the lock of the page it
// spent fails too. The page is left spent and unlocked, the result says so, and the next run
// locks it before writing the secret elsewhere.
TEST(a_bad_write_and_then_a_bad_lock) {
    rootburn_report_t rep;
    fresh();
    fail_write_at = 5;                                  // the sixth secret row
    silent_write_at = 6;                                // and then the lock of the spent page
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_WRITE);
    CHECK(!rep.page_locked && !rep.nothing_landed);
    CHECK(!rows_blank(0) && !locked(0));
    run_clean(P(1));
    CHECK(locked(0));
}

// Power lost after n programs have landed: nothing after them does, the lock included. When
// power is back, one clean run gives the board its secret and locks everything written.
TEST(power_lost_anywhere_then_run_again) {
    for (int n = 0; n <= PROGRAMS; n++) {
        rootburn_report_t rep;
        unsigned index;
        uint8_t s[ROOTOTP_SECRET_LEN];
        fresh();
        cut_at = n;
        rootburn_run(&ops, false, &rep);
        roototp_find_t f = roototp_find(rd, &index);
        if (n >= PROGRAMS - 1) {                        // the marker landed
            CHECK_EQ(f, ROOTOTP_FOUND);
            CHECK(roototp_read(rd, index, s));
        } else {
            CHECK(f != ROOTOTP_FOUND);
        }
        no_faults();
        rootburn_run(&ops, false, &rep);
        if (n == PROGRAMS)                              // nothing was lost at all
            CHECK_EQ(rep.result, ROOTBURN_HAS_SECRET);
        else if (n == PROGRAMS - 1)                     // only the lock was lost
            CHECK_EQ(rep.result, ROOTBURN_RELOCKED);
        else {
            CHECK_EQ(rep.result, ROOTBURN_OK);
            CHECK_EQ(rep.page, n == 0 ? P(0) : P(1));
        }
        check_invariant(true);
    }
    // The same for a rehearsal cut short, then the rehearsal again, then the real run.
    for (int n = 1; n <= 32; n += 7) {
        rootburn_report_t rep;
        fresh();
        cut_at = n;
        rootburn_run(&ops, true, &rep);
        no_faults();
        rootburn_run(&ops, true, &rep);
        CHECK_EQ(rep.result, ROOTBURN_REHEARSED);
        CHECK_EQ(rep.page, P(1));
        CHECK(locked(0) && locked(1));
        run_clean(P(2));
    }
}

// A page with a stray bit in it isn't blank: it is spent, locked, and the next one is used.
TEST(a_page_with_a_stray_bit) {
    rootburn_report_t rep;
    fresh();
    otp[ROOTOTP_ROW(0, ROOTOTP_CHAFF_ROW + 2)] = 0x000400;
    rootburn_run(&ops, false, &rep);
    CHECK_EQ(rep.result, ROOTBURN_OK);
    CHECK_EQ(rep.page, P(1));
    CHECK(locked(0));
    check_invariant(true);
}

// When to stop and look before running anything again.
TEST(when_to_stop) {
    CHECK(!rootburn_must_stop(ROOTBURN_OK));
    CHECK(!rootburn_must_stop(ROOTBURN_REHEARSED));
    CHECK(!rootburn_must_stop(ROOTBURN_RELOCKED));
    CHECK(!rootburn_must_stop(ROOTBURN_HAS_SECRET));
    CHECK(!rootburn_must_stop(ROOTBURN_NOT_PRESENT));
    CHECK(rootburn_must_stop(ROOTBURN_NO_PAGE));
    CHECK(rootburn_must_stop(ROOTBURN_EARLIER_LOCK));
    CHECK(rootburn_must_stop(ROOTBURN_RANDOM));
    CHECK(rootburn_must_stop(ROOTBURN_WRITE));
    CHECK(rootburn_must_stop(ROOTBURN_VERIFY));
    CHECK(rootburn_must_stop(ROOTBURN_LOCK));
    for (int r = ROOTBURN_OK; r <= ROOTBURN_LOCK; r++)
        CHECK(strlen(rootburn_result_text((rootburn_result_t)r)) > 10);
    CHECK_EQ(ROOTBURN_LOCK_WORD, 0x3d3d3d);
    CHECK(!roototp_bootloader_can_write(ROOTBURN_LOCK_WORD));
    CHECK(!roototp_secure_can_write(ROOTBURN_LOCK_WORD));
    CHECK(roototp_secure_can_write(0));
    CHECK(roototp_secure_can_write(0x303030));          // bootloader locked only
    CHECK(roototp_secure_can_write(0x000001));          // one copy of three: no effect
    CHECK(!roototp_secure_can_write(0x010100));
    CHECK(!roototp_secure_can_write(0xffffffffu));      // can't be read: don't count on it
}

// The model itself: it must refuse what the chip refuses, or the tests above prove nothing.
TEST(the_model_refuses_what_the_chip_refuses) {
    fresh();
    CHECK(f_write_raw(NULL, 0x100, 0x00000f));
    CHECK(!f_write_raw(NULL, 0x100, 0x0000f0));         // would leave set bits unset
    CHECK(f_write_raw(NULL, 0x100, 0x0000ff));
    CHECK(f_write_ecc(NULL, 0x101, 0x1234));
    CHECK(!f_write_ecc(NULL, 0x101, 0x4321));           // a written row can't be rewritten
    CHECK(f_write_ecc(NULL, 0x101, 0x1234));
    CHECK(f_write_lock1(NULL, 4, 0x010101));            // Secure read-only
    CHECK(!f_write_raw(NULL, 0x102, 1));                // now nothing in the page can be written
    CHECK(!f_write_ecc(NULL, 0x103, 1));
    CHECK(!f_write_lock1(NULL, 4, 0x3d3d3d));           // nor its lock word
    CHECK(f_write_raw(NULL, 0x140, 1));                 // another page is unaffected
}

TEST_MAIN(
    RUN(the_model_refuses_what_the_chip_refuses);
    RUN(a_clean_burn);
    RUN(nothing_is_written_twice);
    RUN(rehearsal_then_the_real_run);
    RUN(a_first_write_that_fails_leaves_the_page_blank_and_unlocked);
    RUN(a_locked_blank_page_is_passed_over);
    RUN(an_unlocked_spent_page_is_locked_first);
    RUN(a_lock_word_with_bits_already_set);
    RUN(nobody_there_means_nothing_written);
    RUN(bad_random_bytes_mean_nothing_written);
    RUN(no_usable_page);
    RUN(a_bad_write_anywhere_then_run_again);
    RUN(a_bad_write_and_then_a_bad_lock);
    RUN(power_lost_anywhere_then_run_again);
    RUN(a_page_with_a_stray_bit);
    RUN(when_to_stop);
)
