// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The board's own readings: the pure part (core/sensors.h).

#include "core/sensors.h"

#include <stdio.h>

// a / b for b > 0, rounded to the nearest, halves away from zero.
static int64_t div_round(int64_t a, int64_t b) {
    return (a >= 0 ? a + b / 2 : a - b / 2) / b;
}

int sensors_temp_tenths(uint32_t sum, unsigned n, uint32_t vref_uv) {
    if (n == 0)
        return SENSORS_NONE;
    // T = 27 + (0.706 V - V) / 0.001721 V, with V = sum * vref / (4096 * n), all scaled by
    // 4096 * n so the division comes last. In range of int64 for any n the board uses.
    int64_t scale = 4096 * (int64_t)n;
    int64_t num = (706000 * scale - (int64_t)sum * vref_uv) * 10;
    return 270 + (int)div_round(num, 1721 * scale);
}

int sensors_vsys_mv(uint32_t sum, unsigned n, uint32_t vref_uv, unsigned divider) {
    if (n == 0)
        return SENSORS_NONE;
    return (int)div_round((int64_t)sum * vref_uv * divider, 4096 * (int64_t)n * 1000);
}

// value / 10 or / 100 with one or two decimals, and the unit. scale is 10 or 100.
static void format_fixed(char *out, size_t out_size, int value, unsigned scale, const char *unit) {
    if (!out_size)
        return;
    if (value == SENSORS_NONE) {
        snprintf(out, out_size, "no reading");
        return;
    }
    unsigned a = value < 0 ? 0u - (unsigned)value : (unsigned)value;
    const char *sign = value < 0 ? "-" : "";
    if (scale == 10)
        snprintf(out, out_size, "%s%u.%u %s", sign, a / 10, a % 10, unit);
    else
        snprintf(out, out_size, "%s%u.%02u %s", sign, a / 100, a % 100, unit);
}

void sensors_format_temp(char *out, size_t out_size, int tenths) {
    format_fixed(out, out_size, tenths, 10, "C");
}

void sensors_format_mv(char *out, size_t out_size, int mv) {
    // To 10 mV: the reading is good to a few percent, and millivolts would suggest more.
    int cv = mv == SENSORS_NONE ? SENSORS_NONE : (int)div_round(mv, 10);
    format_fixed(out, out_size, cv, 100, "V");
}

void sensors_format_both(char *out, size_t out_size, int tenths, int mv) {
    if (!out_size)
        return;
    char t[SENSORS_TEXT_MAX], v[SENSORS_TEXT_MAX];
    sensors_format_temp(t, sizeof(t), tenths);
    sensors_format_mv(v, sizeof(v), mv);
    if (mv != SENSORS_NONE)
        snprintf(out, out_size, "chip %s, supply %s", t, v);
    else if (tenths != SENSORS_NONE)
        snprintf(out, out_size, "chip %s", t);
    else
        snprintf(out, out_size, "no readings");
}

void sensors_format_age(char *out, size_t out_size, uint32_t s) {
    if (!out_size)
        return;
    unsigned long v = s;
    if (v < 60)
        snprintf(out, out_size, "%lu s", v);
    else if (v < 3600)
        snprintf(out, out_size, "%lu min", v / 60);
    else if (v < 86400)
        snprintf(out, out_size, "%lu h %02lu min", v / 3600, v / 60 % 60);
    else
        snprintf(out, out_size, "%lu d %02lu h", v / 86400, v / 3600 % 24);
}

void sensors_stats_add(sensors_stats_t *st, int value, uint32_t now_s) {
    if (value == SENSORS_NONE)
        return;
    // Strictly lower or higher: the time shown is when the extreme was first reached.
    if (!st->count || value < st->min) {
        st->min = value;
        st->min_s = now_s;
    }
    if (!st->count || value > st->max) {
        st->max = value;
        st->max_s = now_s;
    }
    st->last = value;
    st->last_s = now_s;
    if (st->count < ~0u)
        st->count++;
}

void sensors_stats_shift(sensors_stats_t *st, int delta) {
    if (!st->count)
        return;
    st->last += delta;
    st->min += delta;
    st->max += delta;
}

bool sensors_parse_temp(const char *s, int *tenths) {
    bool neg = false;
    if (*s == '-' || *s == '+')
        neg = *s++ == '-';
    if (*s < '0' || *s > '9')
        return false;
    int v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
        if (v > 1000)                   // well past the range: stop before it can overflow
            return false;
    }
    v *= 10;
    if (*s == '.') {
        s++;
        if (*s < '0' || *s > '9')
            return false;
        v += *s++ - '0';
    }
    if (*s)
        return false;
    if (neg)
        v = -v;
    if (v < SENSORS_CAL_MIN || v > SENSORS_CAL_MAX)
        return false;
    *tenths = v;
    return true;
}

static bool moved(int now, int then, int step) {
    if (now == SENSORS_NONE || then == SENSORS_NONE)
        return now != then;             // a reading appeared or went away
    int d = now - then;
    return (d < 0 ? -d : d) >= step;
}

bool sensors_log_due(const sensors_logged_t *lg, int temp, int mv, int usb, uint32_t now_s) {
    if (temp == SENSORS_NONE)
        return false;
    if (!lg->any || usb != lg->usb)
        return true;
    uint32_t since = now_s - lg->s;
    if (since >= SENSORS_LOG_MAX_S)
        return true;
    return since >= SENSORS_LOG_MIN_S && (moved(temp, lg->temp, SENSORS_LOG_STEP_TEMP) ||
                                          moved(mv, lg->mv, SENSORS_LOG_STEP_MV));
}

void sensors_log_mark(sensors_logged_t *lg, int temp, int mv, int usb, uint32_t now_s) {
    lg->any = true;
    lg->s = now_s;
    lg->temp = temp;
    lg->mv = mv;
    lg->usb = usb;
}

uint32_t sensors_pack(int value, uint32_t marker) {
    // SENSORS_NONE is INT16_MIN; every real reading fits in 16 bits (the temperature sensor
    // gives -1480.3 to 437.2 degrees at 3.3 V, VSYS at most 9.9 V).
    if (value < INT16_MIN)
        value = INT16_MIN;
    if (value > INT16_MAX)
        value = INT16_MAX;
    return (marker & 0xffffu) << 16 | ((uint32_t)value & 0xffffu);
}

int sensors_unpack(uint32_t reg, uint32_t marker) {
    if (reg >> 16 != (marker & 0xffffu))
        return SENSORS_NONE;
    int v = (int)(reg & 0xffffu);
    return v >= 0x8000 ? v - 0x10000 : v;
}
