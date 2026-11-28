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

// What mktime() gives for a `struct tm` when the time zone is UTC, without the C library's time
// zone code (which reads TZ with sscanf and so links scanf): `year` is years since 1900 and
// `mon` is 0-11, as in struct tm. Fields outside their range carry over as mktime()'s do (month
// 12 is January of the next year, day 0 is the last day of the month before).
int64_t timeconv_from_tm(int year, int mon, int mday, int hour, int min, int sec);

// PCF8523 registers 03h..09h (Seconds, Minutes, Hours, Days, Weekdays, Months, Years), 24-hour
// mode. The OS flag (bit 7 of Seconds) is ignored when decoding and written as 0.
#define TIMECONV_RTC_REGS 7

// Decode the registers. Returns false if a field isn't valid BCD, is out of range, or names a
// date that doesn't exist.
bool timeconv_from_rtc(const uint8_t regs[TIMECONV_RTC_REGS], int64_t *t);

// Encode t. Returns false if t is outside TIMECONV_YEAR_MIN..MAX.
bool timeconv_to_rtc(int64_t t, uint8_t regs[TIMECONV_RTC_REGS]);

// RTC calibration. The PCF8523's offset register (0Eh) holds -64..63 steps; in its default mode
// (a correction every two hours) one step changes the RTC's rate by 4.34 ppm, and a positive
// value slows it. Rates here are in thousandths of a ppm.
#define TIMECONV_CAL_STEP_MPPM 4340
#define TIMECONV_CAL_STEPS_MIN (-64)
#define TIMECONV_CAL_STEPS_MAX 63

// Parse a rate in ppm: an optional sign, up to 4 digits, and up to 3 decimals ("92", "-12.5").
bool timeconv_parse_ppm(const char *s, int32_t *mppm);

// The number of steps nearest to a rate. Returns false if the rate is beyond what the register
// can do (-277.76 to 273.42 ppm).
bool timeconv_cal_steps(int32_t mppm, int *steps);

// --- drift: the RTC's and the crystal's rates against NTS (docs/design/health.md §9.5) ----------

typedef enum {
    TIMECONV_DRIFT_NO_BASELINE,     // no usable record of when the RTC was last set
    TIMECONV_DRIFT_BASELINE,        // set at a sync (rtc_interval_s: how long ago, 0 unknown)
    TIMECONV_DRIFT_CAL_CHANGED,     // the last sync couldn't measure: the calibration had changed
    TIMECONV_DRIFT_MEASURED,        // the last sync measured it
    TIMECONV_DRIFT_LOAD_CHANGED,    // ...couldn't measure: the crystal load had changed (or the
                                    // baseline's firmware didn't record it)
} timeconv_drift_state_t;

typedef struct {
    bool rtc_present;               // the RTC answered at the last look
    timeconv_drift_state_t rtc;
    int64_t rtc_err_ms;             // MEASURED: the RTC's reading minus NTS time; positive = fast
    uint32_t rtc_unc_ms;            // ...its uncertainty
    uint32_t rtc_interval_s;        // MEASURED: since the RTC was set; BASELINE: its age
    int cal_steps;                  // the calibration in force (TIMECONV_CAL_* steps)
    uint8_t load;                   // the RTC crystal's load in force, tenths of a pF (0 unknown)
    bool xosc;                      // two syncs in one uptime: the crystal measured
    int64_t xosc_err_ms;            // the system clock's lead over NTS (minus the step)
    uint32_t xosc_unc_ms;
    uint32_t xosc_interval_s;
} timeconv_drift_t;

#define TIMECONV_RTC_TICK_MS     1000   // the RTC counts whole seconds
// The PCF8523 applies its correction once every two hours (mode 0), so its error swings by up to
// one step's 4.34 ppm over 7200 s, 31.25 ms, per step: rounded up.
#define TIMECONV_CAL_SAWTOOTH_MS 32

// The uncertainty of one RTC measurement: the whole second, the calibration's sawtooth, and the
// sync's round trip (half of each of two syncs').
uint32_t timeconv_rtc_unc_ms(int cal_steps, uint32_t delay_ms);

// err_ms over interval_s as thousandths of a ppm; 0 for an interval of 0.
int64_t timeconv_rate_mppm(int64_t err_ms, uint32_t interval_s);

// Judged only when sure: 1 if |rate| - unc is over limit, -1 if |rate| + unc is within it, 0 if
// the uncertainty straddles it ("too short to judge").
int timeconv_verdict(int64_t rate_mppm, int64_t unc_mppm, int64_t limit_mppm);

// What a calibration of `steps` does to the RTC's rate, in thousandths of a ppm; positive slows
// it, as `time cal` takes it.
int32_t timeconv_cal_mppm(int steps);

// Whether an RTC baseline set under (base_cal, base_load) can be measured from under (cal, load):
// TIMECONV_DRIFT_BASELINE if both are the same, else what changed, the load first (a base_load of
// 0, not recorded, is a change). Loads in tenths of a pF.
timeconv_drift_state_t timeconv_baseline(int base_cal, uint8_t base_load, int cal, uint8_t load);

// Texts for the check and `time`: "+92.1+-13 ppm" (one decimal under 100 ppm, the uncertainty
// with one decimal under 10); "no cal" or "cal 91.1"; "45 min", "22 h", "3 d".
void timeconv_format_rate(char *buf, unsigned len, int64_t rate_mppm, int64_t unc_mppm);
void timeconv_format_cal(char *buf, unsigned len, int steps);
// A crystal load in tenths of a pF: "12.5 pF", "7 pF"; "load not recorded" for 0.
void timeconv_format_load(char *buf, unsigned len, uint8_t load);
// The calibration and the load: "no cal, 12.5 pF"; the calibration alone if the load is 0.
void timeconv_format_setup(char *buf, unsigned len, int steps, uint8_t load);
void timeconv_format_span(char *buf, unsigned len, uint32_t s);
// A rate in ppm with one decimal, the ".0" dropped: "92", "-13.4", "114.5".
void timeconv_format_ppm(char *buf, unsigned len, int64_t mppm);

// "2026-11-23 23:30:05" (UTC) into buf (at least 20 bytes).
void timeconv_format(int64_t t, char *buf, unsigned buflen);

#endif
