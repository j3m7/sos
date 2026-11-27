// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The board's own readings: the hardware (core/sensors.h).
//
// Nothing else in SOS uses the ADC. Each reading selects its own input. Everything runs on
// core 0 (the main loop and the shell); Lua on core 1 has no ADC access.
//
// VSYS shares its pin with the Wi-Fi chip's SPI clock (Pico 2 W datasheet, "Wireless
// interface"): the divider reaches the pin through a FET that conducts only while the Wi-Fi
// chip is deselected, so VSYS can be read only between SPI transfers. Holding the Wi-Fi
// driver's lock (cyw43_thread_enter) keeps transfers out, as the SDK's board header and
// Raspberry Pi's read_vsys example do. The pin is switched to the ADC (no function, no pulls,
// input off); the driver gives it back to PIO, with its pull-down, at the start of every
// transfer (cyw43_bus_pio_spi.c, start_spi_comms). Tried on dev-1 at 45 readings a second
// under heavy ping traffic: no more packets lost than without the readings.

#include "core/sensors.h"

#include "hardware/adc.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "core/log.h"

static sensors_stats_t temp_stats, vsys_stats;
static sensors_logged_t logged;
static int boot_temp = SENSORS_NONE;    // sensors_init's, before Wi-Fi starts
static int usb = -1;
static uint64_t next_sample_us;

static uint32_t uptime_s(void) {
    return (uint32_t)(time_us_64() / 1000000);
}

static uint32_t sum_conversions(void) {
    uint32_t sum = 0;
    for (unsigned i = 0; i < SENSORS_SAMPLES; i++)
        sum += adc_read();
    return sum;
}

static int sample_temp(void) {
    // The sensor's own channel needs no settling after another channel: on dev-1 the first
    // conversion after a VSYS reading matched the settled ones.
    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
    return sensors_temp_tenths(sum_conversions(), SENSORS_SAMPLES, SENSORS_VREF_UV);
}

int sensors_read_temp(void) {
    int t = sample_temp();
    sensors_stats_add(&temp_stats, t, uptime_s());
    return t;
}

int sensors_read_vsys(void) {
#if defined(PICO_VSYS_PIN) && defined(CYW43_WL_GPIO_VBUS_PIN)
    cyw43_thread_enter();
    // USB power, from the Wi-Fi chip's VBUS sense pin. Asking also wakes the Wi-Fi chip, which
    // Raspberry Pi's read_vsys example does before reading VSYS.
    usb = cyw43_arch_gpio_get(CYW43_WL_GPIO_VBUS_PIN) ? 1 : 0;
    adc_gpio_init(PICO_VSYS_PIN);
    adc_select_input(PICO_VSYS_PIN - ADC_BASE_PIN);
    for (unsigned i = 0; i < SENSORS_VSYS_DISCARD; i++)
        (void)adc_read();
    uint32_t sum = sum_conversions();
    cyw43_thread_exit();
    int mv = sensors_vsys_mv(sum, SENSORS_SAMPLES, SENSORS_VREF_UV, SENSORS_VSYS_DIVIDER);
    sensors_stats_add(&vsys_stats, mv, uptime_s());
    return mv;
#else
    return SENSORS_NONE;
#endif
}

void sensors_init(void) {
    adc_init();
    adc_set_temp_sensor_enabled(true);
    // The sensor's bias takes a moment to settle: read at once, it says the chip is a few
    // degrees warmer than it is.
    busy_wait_us(SENSORS_TEMP_SETTLE_US);
    // Kept for the glitch detector until the main loop's first reading, but not counted in the
    // lowest and highest: taken before the Wi-Fi chip starts, it reads 0.6 to 0.8 degree above
    // every reading after (dev-1, four boots), most likely because the Wi-Fi chip's load moves
    // the 3.3 V supply that is the ADC's reference. Counted, it would be "highest since boot".
    boot_temp = sample_temp();
}

void sensors_poll(void) {
    uint64_t now = time_us_64();
    if (now < next_sample_us)
        return;
    next_sample_us = now + SENSORS_SAMPLE_S * 1000000ull;
    int t = sensors_read_temp(), mv = sensors_read_vsys();
    uint32_t up = uptime_s();
    if (!sensors_log_due(&logged, t, mv, usb, up))
        return;
    char t_now[SENSORS_TEXT_MAX], t_lo[SENSORS_TEXT_MAX], t_hi[SENSORS_TEXT_MAX];
    char v_now[SENSORS_TEXT_MAX], v_lo[SENSORS_TEXT_MAX], v_hi[SENSORS_TEXT_MAX];
    sensors_format_temp(t_now, sizeof(t_now), t);
    sensors_format_temp(t_lo, sizeof(t_lo), temp_stats.min);
    sensors_format_temp(t_hi, sizeof(t_hi), temp_stats.max);
    sensors_format_mv(v_now, sizeof(v_now), mv);
    sensors_format_mv(v_lo, sizeof(v_lo), vsys_stats.count ? vsys_stats.min : SENSORS_NONE);
    sensors_format_mv(v_hi, sizeof(v_hi), vsys_stats.count ? vsys_stats.max : SENSORS_NONE);
    log_msg(SOS_LOG_KERN | SOS_LOG_NOTICE,
            "chip temperature %s, supply %s, %s (since boot: %s to %s, %s to %s)",
            t_now, v_now, usb > 0 ? "USB power" : usb == 0 ? "no USB power" : "USB power unknown",
            t_lo, t_hi, v_lo, v_hi);
    sensors_log_mark(&logged, t, mv, usb, up);
}

int sensors_last_temp(void) {
    return temp_stats.count ? temp_stats.last : boot_temp;
}

int sensors_last_vsys(void) {
    return vsys_stats.count ? vsys_stats.last : SENSORS_NONE;
}

int sensors_usb_power(void) {
    return usb;
}

const sensors_stats_t *sensors_temp_stats(void) {
    return &temp_stats;
}

const sensors_stats_t *sensors_vsys_stats(void) {
    return &vsys_stats;
}
