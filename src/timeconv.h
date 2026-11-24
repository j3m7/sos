// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Time conversions for the RTC and the clock: Unix time <-> calendar (UTC), and calendar <->
// PCF8523 time registers (BCD).
//
// Pure C: no Pico SDK dependency, so it runs in the host tests.

#ifndef SOS_TIMECONV_H
#define SOS_TIMECONV_H

#include <stdbool.h>
#include <stdint.h>

// The PCF8523 stores a two-digit year; SOS reads it as 2000-2099.
#define TIMECONV_YEAR_MIN 2000
#define TIMECONV_YEAR_MAX 2099

typedef struct {
    int year;       // e.g. 2026
    int month;      // 1-12
    int day;        // 1-31
    int hour;       // 0-23
    int min;        // 0-59
    int sec;        // 0-59
    int wday;       // 0-6, 0 = Sunday
} timeconv_cal_t;

// Days since 1970-01-01 for a proleptic Gregorian date (negative before 1970).
int64_t timeconv_days_from_civil(int year, int month, int day);

// Unix time (seconds since 1970-01-01 00:00:00 UTC) -> calendar, UTC.
void timeconv_to_cal(int64_t t, timeconv_cal_t *cal);

// Calendar -> Unix time. Returns false if any field is out of range or the day doesn't exist
// in that month (wday is ignored).
bool timeconv_from_cal(const timeconv_cal_t *cal, int64_t *t);

// PCF8523 registers 03h..09h (Seconds, Minutes, Hours, Days, Weekdays, Months, Years), 24-hour
// mode. The OS flag (bit 7 of Seconds) is ignored when decoding and written as 0.
#define TIMECONV_RTC_REGS 7

// Decode the registers. Returns false if a field isn't valid BCD, is out of range, or names a
// date that doesn't exist.
bool timeconv_from_rtc(const uint8_t regs[TIMECONV_RTC_REGS], int64_t *t);

// Encode t. Returns false if t is outside TIMECONV_YEAR_MIN..MAX.
bool timeconv_to_rtc(int64_t t, uint8_t regs[TIMECONV_RTC_REGS]);

// "2026-11-23 23:30:05" (UTC) into buf (at least 20 bytes).
void timeconv_format(int64_t t, char *buf, unsigned buflen);

#endif
