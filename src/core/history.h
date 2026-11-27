// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The history sector (docs/design/health.md §9.5): one record per boot (why the chip reset, how
// long it had been up, the time) and one per time sync that set the RTC (the drift check's
// baseline, time/timesync.c). Kept in flash, the sixth sector from the end (core/flashlayout.h),
// so it survives what the POWMAN scratch registers don't: a power cycle, a brownout, the reset
// pin.
//
// Written the failcount way: records are programmed into erased flash in order, and the sector is
// erased only when full, the newest records copied back. Page 0 holds a header with the format's
// version; firmware that finds a newer version than HISTORY_VERSION never writes the sector, so a
// later format can't be damaged by a downgrade. Older firmware never touches the sector at all.
//
// history.c is pure (host tested) and reaches flash through a history_store_t;
// history_board.c is the flash backend and the boot-time record.

#ifndef SOS_HISTORY_H
#define SOS_HISTORY_H

#include <stdbool.h>
#include <stdint.h>

#define HISTORY_SECTOR  4096
#define HISTORY_PAGE    256
#define HISTORY_RECORD  16
#define HISTORY_SLOTS   ((HISTORY_SECTOR - HISTORY_PAGE) / HISTORY_RECORD)     // 240
#define HISTORY_VERSION 1
#define HISTORY_CARRY_BOOTS 16          // boot records copied back when the sector is erased
#define HISTORY_NONE    0xffffffffu     // an uptime or a time not known

typedef enum {
    HISTORY_KIND_BOOT = 1,
    HISTORY_KIND_RTC = 2,
    HISTORY_KIND_TEST = 0x7f,           // `health history-fill` (test builds): shown, never judged
} history_kind_t;

// One record. Bytes on flash: 0 kind (0xff free); 1 reason (boot: resetwhy_t) or the RTC
// calibration's steps (int8); 2-3 the boot number (wraps); 4-7 the uptime before the reset, s;
// 8-11 the wall time (boot: at boot; RTC: what was written to it), Unix s; 12 extra (boot: the
// glitch detectors fired); 13 0xff, programmed to 0 once confirmed (an RTC record, after the RTC
// took the time; outside the CRC); 14-15 CRC-16/CCITT over bytes 0-12, little-endian.
typedef struct {
    uint8_t kind;
    uint8_t reason;                     // boot: resetwhy_t
    int8_t cal_steps;                   // RTC: the calibration in force (the same byte as reason)
    uint16_t boot_no;
    uint32_t uptime_s;                  // HISTORY_NONE: unknown
    uint32_t wall;                      // HISTORY_NONE: the clock wasn't set
    uint8_t extra;
    bool confirmed;
} history_rec_t;

typedef struct {
    void *ctx;
    // The sector as it is now, HISTORY_SECTOR bytes; NULL if there is none to use.
    const uint8_t *(*read)(void *ctx);
    // Program one page: bits go from 1 to 0 only. Erase: every bit 1.
    bool (*program)(void *ctx, unsigned page, const uint8_t *data);
    bool (*erase)(void *ctx);
} history_store_t;

typedef enum {
    HISTORY_ABSENT,             // no sector (the image overlaps it)
    HISTORY_ERASED,             // all 0xff: nothing recorded yet
    HISTORY_OURS,               // our header and version
    HISTORY_NEWER,              // a newer version's header: read nothing, write nothing
    HISTORY_FOREIGN,            // neither (old image data, say): erased before the first record
} history_state_t;

typedef struct {
    history_state_t state;
    unsigned next;              // the first free slot (HISTORY_SLOTS: full)
    unsigned valid;             // records before it with a right CRC and a known kind
    unsigned boots;             // ...of them boot records
    unsigned damaged;           // records before it that are neither
    unsigned stray;             // slots after it that aren't free
} history_scan_t;

void history_scan(const uint8_t *sector, history_scan_t *out);

// One slot (OURS only): false for a free or damaged slot.
bool history_get(const uint8_t *sector, unsigned slot, history_rec_t *out);
// The same without checking the sector first (the caller scanned it): for a walk over the slots.
bool history_slot(const uint8_t *sector, unsigned slot, history_rec_t *out);

// Up to max valid records of a kind (0: every kind), newest first. Returns how many.
unsigned history_list(const uint8_t *sector, uint8_t kind, history_rec_t *out, unsigned max);

// The boot number the next boot record gets: one after the newest's, 0 if none.
uint16_t history_next_boot_no(const uint8_t *sector);

// Append a record: writes the header first into an erased or foreign sector; when the sector is
// full, erases it and copies back the newest HISTORY_CARRY_BOOTS boot records and the newest
// confirmed RTC record. Every write is read back. Returns the slot written, or -1 (no sector, a
// newer version's, or a write that didn't take). Never touches a NEWER sector.
int history_append(const history_store_t *store, const history_rec_t *rec);

// Mark the RTC record in `slot` confirmed (byte 13 to 0). False if it didn't take.
bool history_confirm(const history_store_t *store, unsigned slot);

// The boot loop rule: of the boot records newest first (boots[0] is this boot's, which holds the
// reset that ended the run before), how many in a row from the newest are resets not asked for
// (not a reboot) that ended a short run: one whose uptime before the reset is under stable_s, or,
// with the uptime unknown, whose start and the next boot's are both timed and under stable_s
// apart. A loop is GLITCHDET_GUARD_RESETS (3) or more.
unsigned history_short_runs(const history_rec_t *boots, unsigned n, uint32_t stable_s);

// Records as bytes, for the tests and the fill command.
void history_encode(const history_rec_t *rec, uint8_t out[HISTORY_RECORD]);

// --- the board (history_board.c) ---------------------------------------------------------------

// In main(), after resetwhy_boot() and timesync_boot() (so the RTC's time is known): append this
// boot's record. Before core 1 starts; one page program (about 1 ms), or an erase when full.
void history_boot(void);

// The sector, NULL if the image overlaps it (then nothing is read or written).
const uint8_t *history_sector(void);

// A time sync is about to write the RTC with `t` under the calibration `cal_steps`: append its
// record, unconfirmed. Returns the slot, or -1. history_rtc_done() confirms it once the RTC has
// taken the time; a record left unconfirmed ends the drift baseline.
int history_rtc_begin(uint32_t t, int cal_steps);
void history_rtc_done(int slot);

#ifdef SOS_TEST_COMMANDS
// `health history-fill`: test records until one slot is left. Returns how many were written.
unsigned history_fill(void);
#endif

#endif
