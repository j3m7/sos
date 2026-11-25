// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "time/timeconv.h"

#include <stdio.h>

// Howard Hinnant's days_from_civil / civil_from_days (public domain algorithms).
int64_t timeconv_days_from_civil(int year, int month, int day) {
    int64_t y = (int64_t)year - (month <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;                                        // [0, 399]
    int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;  // [0, 365]
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                // [0, 146096]
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *year, int *month, int *day) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *day = (int)(doy - (153 * mp + 2) / 5 + 1);
    *month = (int)(mp < 10 ? mp + 3 : mp - 9);
    *year = (int)(yoe + era * 400 + (*month <= 2));
}

static int days_in_month(int year, int month) {
    static const uint8_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)))
        return 29;
    return days[month - 1];
}

void timeconv_to_cal(int64_t t, timeconv_cal_t *cal) {
    int64_t days = t / 86400;
    int64_t secs = t % 86400;
    if (secs < 0) {
        secs += 86400;
        days--;
    }
    civil_from_days(days, &cal->year, &cal->month, &cal->day);
    cal->hour = (int)(secs / 3600);
    cal->min = (int)(secs / 60 % 60);
    cal->sec = (int)(secs % 60);
    int64_t w = (days + 4) % 7;     // 1970-01-01 was a Thursday
    cal->wday = (int)(w < 0 ? w + 7 : w);
}

bool timeconv_from_cal(const timeconv_cal_t *cal, int64_t *t) {
    if (cal->month < 1 || cal->month > 12 || cal->day < 1 ||
        cal->day > days_in_month(cal->year, cal->month) ||
        cal->hour < 0 || cal->hour > 23 || cal->min < 0 || cal->min > 59 ||
        cal->sec < 0 || cal->sec > 59)
        return false;
    *t = timeconv_days_from_civil(cal->year, cal->month, cal->day) * 86400 +
         cal->hour * 3600 + cal->min * 60 + cal->sec;
    return true;
}

// BCD byte (only the bits in mask) -> value, or -1 if a digit is over 9.
static int bcd_decode(uint8_t b, uint8_t mask) {
    b &= mask;
    if ((b & 0x0f) > 9 || (b >> 4) > 9)
        return -1;
    return (b >> 4) * 10 + (b & 0x0f);
}

static uint8_t bcd_encode(int v) {
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

bool timeconv_from_rtc(const uint8_t regs[TIMECONV_RTC_REGS], int64_t *t) {
    timeconv_cal_t cal;
    cal.sec = bcd_decode(regs[0], 0x7f);
    cal.min = bcd_decode(regs[1], 0x7f);
    cal.hour = bcd_decode(regs[2], 0x3f);
    cal.day = bcd_decode(regs[3], 0x3f);
    cal.wday = regs[4] & 0x07;
    cal.month = bcd_decode(regs[5], 0x1f);
    int yy = bcd_decode(regs[6], 0xff);
    if (cal.sec < 0 || cal.min < 0 || cal.hour < 0 || cal.day < 0 || cal.month < 0 || yy < 0)
        return false;
    cal.year = TIMECONV_YEAR_MIN + yy;
    return timeconv_from_cal(&cal, t);
}

bool timeconv_to_rtc(int64_t t, uint8_t regs[TIMECONV_RTC_REGS]) {
    timeconv_cal_t cal;
    timeconv_to_cal(t, &cal);
    if (cal.year < TIMECONV_YEAR_MIN || cal.year > TIMECONV_YEAR_MAX)
        return false;
    regs[0] = bcd_encode(cal.sec);      // OS = 0
    regs[1] = bcd_encode(cal.min);
    regs[2] = bcd_encode(cal.hour);
    regs[3] = bcd_encode(cal.day);
    regs[4] = (uint8_t)cal.wday;
    regs[5] = bcd_encode(cal.month);
    regs[6] = bcd_encode(cal.year - TIMECONV_YEAR_MIN);
    return true;
}

bool timeconv_parse_ppm(const char *s, int32_t *mppm) {
    bool negative = *s == '-';
    if (*s == '-' || *s == '+')
        s++;
    int32_t whole = 0, frac = 0;
    int digits = 0;
    for (; *s >= '0' && *s <= '9'; s++, digits++)
        whole = whole * 10 + (*s - '0');
    if (digits < 1 || digits > 4)
        return false;
    if (*s == '.') {
        int scale = 100;
        s++;
        for (digits = 0; *s >= '0' && *s <= '9'; s++, digits++, scale /= 10)
            frac += (*s - '0') * scale;
        if (digits < 1 || digits > 3)
            return false;
    }
    if (*s)
        return false;
    *mppm = (whole * 1000 + frac) * (negative ? -1 : 1);
    return true;
}

bool timeconv_cal_steps(int32_t mppm, int *steps) {
    // Rounded to the nearest step, halves away from zero.
    int32_t half = TIMECONV_CAL_STEP_MPPM / 2;
    int32_t n = (mppm >= 0 ? mppm + half : mppm - half) / TIMECONV_CAL_STEP_MPPM;
    if (n < TIMECONV_CAL_STEPS_MIN || n > TIMECONV_CAL_STEPS_MAX)
        return false;
    *steps = (int)n;
    return true;
}

void timeconv_format(int64_t t, char *buf, unsigned buflen) {
    timeconv_cal_t cal;
    timeconv_to_cal(t, &cal);
    snprintf(buf, buflen, "%04d-%02d-%02d %02d:%02d:%02d",
             cal.year, cal.month, cal.day, cal.hour, cal.min, cal.sec);
}
