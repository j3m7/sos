// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/history.c, the history sector's format (docs/design/health.md §9.5):
// appending, the header, a full sector erased with the newest records copied back, damaged and
// stray records, a newer version never written, RTC records and their confirmation, and the
// boot loop rule. The flash is a buffer that, like NOR flash, only clears bits when programmed.

#include <string.h>

#include "core/history.h"
#include "core/resetwhy.h"
#include "test.h"

static uint8_t flash[HISTORY_SECTOR];
static unsigned erases, programs;
static bool fail_program;

static const uint8_t *f_read(void *ctx) { (void)ctx; return flash; }

static bool f_program(void *ctx, unsigned page, const uint8_t *data) {
    (void)ctx;
    if (fail_program || page >= HISTORY_SECTOR / HISTORY_PAGE)
        return false;
    programs++;
    for (unsigned i = 0; i < HISTORY_PAGE; i++)
        flash[page * HISTORY_PAGE + i] &= data[i];
    return true;
}

static bool f_erase(void *ctx) {
    (void)ctx;
    erases++;
    memset(flash, 0xff, sizeof(flash));
    return true;
}

static const history_store_t store = {NULL, f_read, f_program, f_erase};

static void reset(void) {
    memset(flash, 0xff, sizeof(flash));
    erases = programs = 0;
    fail_program = false;
}

static history_rec_t boot(resetwhy_t why, uint16_t no, uint32_t uptime, uint32_t wall) {
    history_rec_t r = {.kind = HISTORY_KIND_BOOT, .reason = (uint8_t)why, .boot_no = no,
                       .uptime_s = uptime, .wall = wall};
    return r;
}

TEST(erased_then_first_record) {
    reset();
    history_scan_t s;
    history_scan(flash, &s);
    CHECK_EQ(s.state, HISTORY_ERASED);
    CHECK_EQ(history_next_boot_no(flash), 0);
    history_rec_t r = boot(RESETWHY_POWER_ON, 0, HISTORY_NONE, 1764000000u);
    CHECK_EQ(history_append(&store, &r), 0);
    history_scan(flash, &s);
    CHECK_EQ(s.state, HISTORY_OURS);
    CHECK_EQ(s.next, 1);
    CHECK_EQ(s.valid, 1);
    CHECK_EQ(s.boots, 1);
    CHECK(!memcmp(flash, "SOSH", 4));
    CHECK_EQ(flash[4], HISTORY_VERSION);
    history_rec_t got;
    CHECK(history_get(flash, 0, &got));
    CHECK_EQ(got.reason, RESETWHY_POWER_ON);
    CHECK_EQ(got.uptime_s, HISTORY_NONE);
    CHECK_EQ(got.wall, 1764000000u);
    CHECK_EQ(history_next_boot_no(flash), 1);
    CHECK(!history_get(flash, 1, &got));            // free
}

TEST(list_newest_first_by_kind) {
    reset();
    for (uint16_t i = 0; i < 5; i++) {
        history_rec_t r = boot(RESETWHY_REBOOT, i, 100u + i, HISTORY_NONE);
        CHECK_EQ(history_append(&store, &r), i * 2);
        history_rec_t t = {.kind = HISTORY_KIND_RTC, .cal_steps = -3, .wall = 1000u + i};
        CHECK_EQ(history_append(&store, &t), i * 2 + 1);
    }
    history_rec_t out[10];
    CHECK_EQ(history_list(flash, HISTORY_KIND_BOOT, out, 10), 5);
    CHECK_EQ(out[0].boot_no, 4);
    CHECK_EQ(out[4].boot_no, 0);
    CHECK_EQ(history_list(flash, HISTORY_KIND_RTC, out, 2), 2);
    CHECK_EQ(out[0].wall, 1004u);
    CHECK_EQ(out[0].cal_steps, -3);
    CHECK(!out[0].confirmed);
    CHECK_EQ(history_list(flash, 0, out, 10), 10);
    CHECK_EQ(out[0].kind, HISTORY_KIND_RTC);
}

TEST(confirm_rtc_record) {
    reset();
    history_rec_t t = {.kind = HISTORY_KIND_RTC, .cal_steps = 21, .wall = 1764000000u};
    int slot = history_append(&store, &t);
    CHECK_EQ(slot, 0);
    CHECK(history_confirm(&store, (unsigned)slot));
    history_rec_t got;
    CHECK(history_get(flash, 0, &got));
    CHECK(got.confirmed);                           // the CRC doesn't cover the confirmation
    CHECK_EQ(got.cal_steps, 21);
    fail_program = true;
    CHECK(!history_confirm(&store, 0));
    CHECK(!history_confirm(&store, HISTORY_SLOTS));
}

TEST(rtc_record_keeps_the_crystal_load) {
    reset();
    history_rec_t t = {.kind = HISTORY_KIND_RTC, .cal_steps = -2, .rtc_load = 125,
                       .wall = 1764000000u};
    CHECK_EQ(history_append(&store, &t), 0);
    uint8_t bytes[HISTORY_RECORD];
    history_encode(&t, bytes);
    CHECK_EQ(bytes[12], 125);                       // byte 12, under the CRC
    history_rec_t got;
    CHECK(history_get(flash, 0, &got));
    CHECK_EQ(got.rtc_load, 125);
    CHECK_EQ(got.cal_steps, -2);
    // A boot record's byte 12 is still its extra.
    history_rec_t b = boot(RESETWHY_GLITCH, 1, 10, HISTORY_NONE);
    b.extra = 0x0c;
    b.rtc_load = 70;                                // not an RTC record: ignored
    CHECK_EQ(history_append(&store, &b), 1);
    CHECK(history_get(flash, 1, &got));
    CHECK_EQ(got.extra, 0x0c);
}

TEST(rtc_record_from_older_firmware) {
    // An RTC record as firmware before #275 wrote it: cal 21 steps, set to 1764000000,
    // confirmed, byte 12 zero. It reads with no load recorded, under the same version.
    static const uint8_t old[HISTORY_RECORD] = {
        0x02, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x81, 0x24, 0x69, 0x00, 0x00, 0x03, 0xdb};
    reset();
    history_rec_t r = boot(RESETWHY_POWER_ON, 0, HISTORY_NONE, HISTORY_NONE);
    CHECK_EQ(history_append(&store, &r), 0);            // the header, version 1
    memcpy(flash + HISTORY_PAGE + HISTORY_RECORD, old, sizeof(old));
    history_scan_t s;
    history_scan(flash, &s);
    CHECK_EQ(s.state, HISTORY_OURS);
    CHECK_EQ(s.valid, 2);
    CHECK_EQ(s.damaged, 0);
    history_rec_t got;
    CHECK_EQ(history_list(flash, HISTORY_KIND_RTC, &got, 1), 1);
    CHECK(got.confirmed);
    CHECK_EQ(got.cal_steps, 21);
    CHECK_EQ(got.wall, 1764000000u);
    CHECK_EQ(got.rtc_load, 0);
    // Our encoding of the same record with no load is the older firmware's, byte for byte.
    uint8_t bytes[HISTORY_RECORD];
    got.rtc_load = 0;
    history_encode(&got, bytes);
    CHECK(!memcmp(bytes, old, sizeof(old)));
}

TEST(full_sector_starts_again_with_the_newest) {
    reset();
    history_rec_t t = {.kind = HISTORY_KIND_RTC, .cal_steps = 5, .rtc_load = 125, .wall = 777u};
    int slot = history_append(&store, &t);
    CHECK(history_confirm(&store, (unsigned)slot));
    history_rec_t u = {.kind = HISTORY_KIND_RTC, .cal_steps = 6, .wall = 888u};   // unconfirmed
    history_append(&store, &u);
    for (unsigned i = 2; i < HISTORY_SLOTS; i++) {
        history_rec_t r = boot(RESETWHY_REBOOT, (uint16_t)i, 50, HISTORY_NONE);
        CHECK_EQ(history_append(&store, &r), (int)i);
    }
    history_scan_t s;
    history_scan(flash, &s);
    CHECK_EQ(s.next, HISTORY_SLOTS);
    CHECK_EQ(erases, 0);
    history_rec_t r = boot(RESETWHY_FAULT, 999, 10, HISTORY_NONE);
    int at = history_append(&store, &r);
    CHECK_EQ(erases, 1);
    // The newest 16 boots and the newest confirmed RTC record (777, not the unconfirmed 888).
    CHECK_EQ(at, HISTORY_CARRY_BOOTS + 1);
    history_scan(flash, &s);
    CHECK_EQ(s.next, HISTORY_CARRY_BOOTS + 2);
    history_rec_t out[20];
    CHECK_EQ(history_list(flash, HISTORY_KIND_RTC, out, 20), 1);
    CHECK_EQ(out[0].wall, 777u);
    CHECK(out[0].confirmed);
    CHECK_EQ(out[0].rtc_load, 125);                 // copied back as it was
    CHECK_EQ(history_list(flash, HISTORY_KIND_BOOT, out, 20), HISTORY_CARRY_BOOTS + 1);
    CHECK_EQ(out[0].boot_no, 999);
    CHECK_EQ(out[1].boot_no, HISTORY_SLOTS - 1);
    CHECK_EQ(out[HISTORY_CARRY_BOOTS].boot_no, HISTORY_SLOTS - HISTORY_CARRY_BOOTS);
}

TEST(damaged_and_stray) {
    reset();
    for (uint16_t i = 0; i < 3; i++) {
        history_rec_t r = boot(RESETWHY_REBOOT, i, 60, HISTORY_NONE);
        history_append(&store, &r);
    }
    flash[HISTORY_PAGE + HISTORY_RECORD + 5] ^= 0x01;   // slot 1's uptime: CRC wrong
    flash[HISTORY_PAGE + 10 * HISTORY_RECORD] = 0x00;     // a stray byte at slot 10
    history_scan_t s;
    history_scan(flash, &s);
    CHECK_EQ(s.next, 3);
    CHECK_EQ(s.valid, 2);
    CHECK_EQ(s.damaged, 1);
    CHECK_EQ(s.stray, 1);
    history_rec_t out[4];
    CHECK_EQ(history_list(flash, HISTORY_KIND_BOOT, out, 4), 2);
    // With a stray record the next append starts again rather than run into it.
    history_rec_t r = boot(RESETWHY_REBOOT, 3, 60, HISTORY_NONE);
    CHECK_EQ(history_append(&store, &r), 2);            // the two good ones copied back
    CHECK_EQ(erases, 1);
    history_scan(flash, &s);
    CHECK_EQ(s.stray, 0);
    CHECK_EQ(s.damaged, 0);
}

TEST(newer_version_never_written) {
    reset();
    history_rec_t r = boot(RESETWHY_REBOOT, 0, 60, HISTORY_NONE);
    history_append(&store, &r);
    memset(flash, 0xff, sizeof(flash)); // a sector a later firmware wrote
    memcpy(flash, "SOSH", 4);
    flash[4] = HISTORY_VERSION + 1;
    unsigned before = programs;
    history_scan_t s;
    history_scan(flash, &s);
    CHECK_EQ(s.state, HISTORY_NEWER);
    CHECK_EQ(history_append(&store, &r), -1);
    CHECK_EQ(programs, before);
    CHECK_EQ(erases, 0);
    history_rec_t out[1];
    CHECK_EQ(history_list(flash, 0, out, 1), 0);
}

TEST(foreign_sector_erased_first) {
    reset();
    memset(flash, 0x5a, 100);
    history_scan_t s;
    history_scan(flash, &s);
    CHECK_EQ(s.state, HISTORY_FOREIGN);
    history_rec_t r = boot(RESETWHY_POWER_ON, 0, HISTORY_NONE, HISTORY_NONE);
    CHECK_EQ(history_append(&store, &r), 0);
    CHECK_EQ(erases, 1);
    history_scan(flash, &s);
    CHECK_EQ(s.state, HISTORY_OURS);
    CHECK_EQ(s.valid, 1);
    history_scan(NULL, &s);
    CHECK_EQ(s.state, HISTORY_ABSENT);
}

TEST(failed_write) {
    reset();
    fail_program = true;
    history_rec_t r = boot(RESETWHY_POWER_ON, 0, HISTORY_NONE, HISTORY_NONE);
    CHECK_EQ(history_append(&store, &r), -1);
}

TEST(boot_loop_rule) {
    history_rec_t b[5];
    // Known uptimes: three faults each under 180 s.
    b[0] = boot(RESETWHY_FAULT, 9, 20, HISTORY_NONE);
    b[1] = boot(RESETWHY_FAULT, 8, 30, HISTORY_NONE);
    b[2] = boot(RESETWHY_FAULT, 7, 179, HISTORY_NONE);
    b[3] = boot(RESETWHY_POWER_ON, 6, HISTORY_NONE, HISTORY_NONE);
    CHECK_EQ(history_short_runs(b, 4, 180), 3);
    b[2].uptime_s = 180;                            // not short
    CHECK_EQ(history_short_runs(b, 4, 180), 2);
    // A reboot asked for breaks the row.
    b[1].reason = RESETWHY_REBOOT;
    CHECK_EQ(history_short_runs(b, 4, 180), 1);
    // Unknown uptimes (brownouts): judged by the wall times of the boots.
    b[0] = boot(RESETWHY_BROWNOUT, 3, HISTORY_NONE, 1000300u);
    b[1] = boot(RESETWHY_BROWNOUT, 2, HISTORY_NONE, 1000200u);
    b[2] = boot(RESETWHY_POWER_ON, 1, HISTORY_NONE, 1000100u);
    b[3] = boot(RESETWHY_POWER_ON, 0, HISTORY_NONE, 1000000u);
    CHECK_EQ(history_short_runs(b, 4, 180), 3);
    CHECK_EQ(history_short_runs(b, 3, 180), 2);    // the oldest has nothing to measure against
    b[2].wall = HISTORY_NONE;                       // the clock wasn't set: can't say
    CHECK_EQ(history_short_runs(b, 4, 180), 1);
    CHECK_EQ(history_short_runs(b, 0, 180), 0);
}

TEST_MAIN(
    RUN(erased_then_first_record);
    RUN(list_newest_first_by_kind);
    RUN(confirm_rtc_record);
    RUN(rtc_record_keeps_the_crystal_load);
    RUN(rtc_record_from_older_firmware);
    RUN(full_sector_starts_again_with_the_newest);
    RUN(damaged_and_stray);
    RUN(newer_version_never_written);
    RUN(foreign_sector_erased_first);
    RUN(failed_write);
    RUN(boot_loop_rule);
)
