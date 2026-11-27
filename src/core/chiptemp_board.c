// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The chip's temperature: reading the sensor (core/chiptemp.h).
//
// Nothing else in SOS uses the ADC, so it is set up here and left on the sensor's channel.
// Everything runs on core 0 (the main loop and the shell); Lua on core 1 has no ADC access.

#include "core/chiptemp.h"

#include "hardware/adc.h"
#include "pico/stdlib.h"

#include "core/log.h"

static chiptemp_stats_t stats;
static uint64_t next_sample_us;

static uint32_t uptime_s(void) {
    return (uint32_t)(time_us_64() / 1000000);
}

int chiptemp_read(void) {
    uint32_t sum = 0;
    for (unsigned i = 0; i < CHIPTEMP_SAMPLES; i++)
        sum += adc_read();
    int t = chiptemp_tenths(sum, CHIPTEMP_SAMPLES, CHIPTEMP_VREF_UV);
    chiptemp_stats_add(&stats, t, uptime_s());
    return t;
}

void chiptemp_init(void) {
    adc_init();
    adc_set_temp_sensor_enabled(true);
    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
    (void)adc_read();                   // the first conversion after switching the sensor on
    chiptemp_read();
}

void chiptemp_poll(void) {
    uint64_t now = time_us_64();
    if (now < next_sample_us)
        return;
    next_sample_us = now + CHIPTEMP_SAMPLE_S * 1000000ull;
    chiptemp_read();
    if (!chiptemp_log_due(&stats))
        return;
    char now_t[CHIPTEMP_TEXT_MAX], lo[CHIPTEMP_TEXT_MAX], hi[CHIPTEMP_TEXT_MAX];
    chiptemp_format(now_t, sizeof(now_t), stats.last);
    chiptemp_format(lo, sizeof(lo), stats.min);
    chiptemp_format(hi, sizeof(hi), stats.max);
    log_msg(SOS_LOG_NOTICE, "chip temperature %s (since boot: lowest %s, highest %s)",
            now_t, lo, hi);
    chiptemp_stats_logged(&stats);
}

int chiptemp_last(void) {
    return stats.count ? stats.last : CHIPTEMP_NONE;
}

const chiptemp_stats_t *chiptemp_stats(void) {
    return &stats;
}
