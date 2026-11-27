// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The chip's temperature: the pure part (core/chiptemp.h).

#include "core/chiptemp.h"

#include <stdio.h>

#define PACK_MAGIC 0x43540000u          // "CT"
#define PACK_HIGH  0xffff0000u

int chiptemp_tenths(uint32_t sum, unsigned n, uint32_t vref_uv) {
    if (n == 0)
        return CHIPTEMP_NONE;
    // T = 27 + (0.706 V - V) / 0.001721 V, with V = sum * vref / (4096 * n), all scaled by
    // 4096 * n so the division comes last. In range of int64 for any n the board uses.
    int64_t scale = 4096 * (int64_t)n;
    int64_t num = (706000 * scale - (int64_t)sum * vref_uv) * 10;
    int64_t den = 1721 * scale;
    int64_t q = (num >= 0 ? num + den / 2 : num - den / 2) / den;
    return 270 + (int)q;
}

void chiptemp_format(char *out, size_t out_size, int tenths) {
    if (!out_size)
        return;
    if (tenths == CHIPTEMP_NONE) {
        snprintf(out, out_size, "no reading");
        return;
    }
    unsigned a = tenths < 0 ? 0u - (unsigned)tenths : (unsigned)tenths;
    snprintf(out, out_size, "%s%u.%u C", tenths < 0 ? "-" : "", a / 10, a % 10);
}

void chiptemp_format_age(char *out, size_t out_size, uint32_t s) {
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

void chiptemp_stats_add(chiptemp_stats_t *st, int tenths, uint32_t now_s) {
    if (tenths == CHIPTEMP_NONE)
        return;
    // Strictly lower or higher: the time shown is when the extreme was first reached.
    if (!st->count || tenths < st->min) {
        st->min = tenths;
        st->min_s = now_s;
    }
    if (!st->count || tenths > st->max) {
        st->max = tenths;
        st->max_s = now_s;
    }
    st->last = tenths;
    st->last_s = now_s;
    if (st->count < ~0u)
        st->count++;
}

bool chiptemp_log_due(const chiptemp_stats_t *st) {
    if (!st->count)
        return false;
    if (!st->logged_any)
        return true;
    uint32_t since = st->last_s - st->logged_s;
    int moved = st->last - st->logged;
    if (moved < 0)
        moved = -moved;
    return since >= CHIPTEMP_LOG_MAX_S ||
           (since >= CHIPTEMP_LOG_MIN_S && moved >= CHIPTEMP_LOG_STEP);
}

void chiptemp_stats_logged(chiptemp_stats_t *st) {
    st->logged_any = true;
    st->logged = st->last;
    st->logged_s = st->last_s;
}

uint32_t chiptemp_pack(int tenths) {
    // Every reading the sensor can give fits in 16 bits (-1480.3 to 437.2 degrees at 3.3 V);
    // CHIPTEMP_NONE is INT16_MIN.
    if (tenths < INT16_MIN)
        tenths = INT16_MIN;
    if (tenths > INT16_MAX)
        tenths = INT16_MAX;
    return PACK_MAGIC | ((uint32_t)tenths & 0xffffu);
}

int chiptemp_unpack(uint32_t reg) {
    if ((reg & PACK_HIGH) != PACK_MAGIC)
        return CHIPTEMP_NONE;
    int v = (int)(reg & 0xffffu);
    return v >= 0x8000 ? v - 0x10000 : v;
}
