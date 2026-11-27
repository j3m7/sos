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
static int boot_reading = CHIPTEMP_NONE;    // chiptemp_init's, before Wi-Fi starts
static uint64_t next_sample_us;

static uint32_t uptime_s(void) {
    return (uint32_t)(time_us_64() / 1000000);
}

static int sample(void) {
    uint32_t sum = 0;
    for (unsigned i = 0; i < CHIPTEMP_SAMPLES; i++)
        sum += adc_read();
    return chiptemp_tenths(sum, CHIPTEMP_SAMPLES, CHIPTEMP_VREF_UV);
}

int chiptemp_read(void) {
    int t = sample();
    chiptemp_stats_add(&stats, t, uptime_s());
    return t;
}

void chiptemp_init(void) {
    adc_init();
    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
    adc_set_temp_sensor_enabled(true);
    // The sensor's bias takes a moment to settle: read at once, it says the chip is a few
    // degrees warmer than it is, and that reading would stay "highest since boot" all run.
    busy_wait_us(CHIPTEMP_SETTLE_US);
    // Kept for the glitch detector until the main loop's first reading, but not counted in the
    // lowest and highest: taken before the Wi-Fi chip starts, it reads 0.6 to 0.8 degree above
    // every reading after (dev-1, four boots), most likely because the Wi-Fi chip's load moves
    // the 3.3 V supply that is the ADC's reference. Counted, it would be "highest since boot".
    boot_reading = sample();
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
    log_msg(SOS_LOG_KERN | SOS_LOG_NOTICE,
            "chip temperature %s (since boot: lowest %s, highest %s)",
            now_t, lo, hi);
    chiptemp_stats_logged(&stats);
}

int chiptemp_last(void) {
    return stats.count ? stats.last : boot_reading;
}

const chiptemp_stats_t *chiptemp_stats(void) {
    return &stats;
}
