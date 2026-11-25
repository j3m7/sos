// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Register details: NXP PCF8523 data sheet, Rev. 7 (docs/peripherals/PCF8523-datasheet.pdf).

#include "time/rtc.h"

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/time.h"

#include "time/timeconv.h"

#define RTC_I2C         i2c0
#define RTC_SDA_PIN     4
#define RTC_SCL_PIN     5
#define RTC_ADDR        0x68
#define RTC_BAUD        100000
#define RTC_TIMEOUT_US  10000       // per transfer; a stuck bus fails instead of hanging

#define REG_CONTROL_1   0x00
#define REG_CONTROL_3   0x02
#define REG_SECONDS     0x03        // 03h..09h: time and date, read/written as one block
#define REG_OFFSET      0x0e        // MODE (bit 7) and a 7-bit two's complement offset

#define CTL1_STOP       0x20        // time circuits frozen
#define CTL1_12_24      0x08        // 12-hour mode
#define CTL3_PM_MASK    0xe0        // PM[2:0]; 000 = switch-over (standard) + battery low detection
#define CTL3_BLF        0x04        // battery low flag
#define SECONDS_OS      0x80        // oscillator stopped: clock integrity not guaranteed
#define OFFSET_MODE     0x80        // 1 = a correction every minute; 0 = every two hours

static bool present;

static bool reg_read(uint8_t reg, uint8_t *buf, size_t len) {
    if (i2c_write_timeout_us(RTC_I2C, RTC_ADDR, &reg, 1, true, RTC_TIMEOUT_US) != 1)
        return false;
    return i2c_read_timeout_us(RTC_I2C, RTC_ADDR, buf, len, false, RTC_TIMEOUT_US) == (int)len;
}

static bool reg_write(uint8_t reg, const uint8_t *buf, size_t len) {
    uint8_t msg[1 + TIMECONV_RTC_REGS];
    if (len > TIMECONV_RTC_REGS)
        return false;
    msg[0] = reg;
    for (size_t i = 0; i < len; i++)
        msg[1 + i] = buf[i];
    return i2c_write_timeout_us(RTC_I2C, RTC_ADDR, msg, len + 1, false, RTC_TIMEOUT_US) ==
           (int)(len + 1);
}

static bool bus_pulled_up;

// The Adalogger pulls SDA and SCL up with 10k; against the RP2350's weak internal pull-downs
// both lines read high if it's there and low if not. (Erratum RP2350-E9 can hold a pad with a
// pull-down high after it was driven high, but pads are reset at boot, so this runs first.)
static bool check_pull_ups(void) {
    const unsigned pins[] = {RTC_SDA_PIN, RTC_SCL_PIN};
    bool high = true;
    for (unsigned i = 0; i < 2; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_pull_down(pins[i]);
    }
    busy_wait_us(100);
    for (unsigned i = 0; i < 2; i++) {
        high = high && gpio_get(pins[i]);
        gpio_disable_pulls(pins[i]);
    }
    return high;
}

bool rtc_bus_pulled_up(void) {
    return bus_pulled_up;
}

bool rtc_init(void) {
    bus_pulled_up = check_pull_ups();
    i2c_init(RTC_I2C, RTC_BAUD);
    gpio_set_function(RTC_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(RTC_SCL_PIN, GPIO_FUNC_I2C);
    // The Adalogger has 10k pull-ups; the internal ones only help if it's missing.
    gpio_pull_up(RTC_SDA_PIN);
    gpio_pull_up(RTC_SCL_PIN);

    uint8_t ctl[3];
    present = reg_read(REG_CONTROL_1, ctl, sizeof(ctl));
    if (!present)
        return false;

    // Control_1: make sure the clock runs and uses 24-hour mode. Leave CAP_SEL (crystal load)
    // as it is. If the chip was in 12-hour mode, the stored hour means something else and
    // rtc_read() will usually report it invalid; the next sync rewrites it.
    uint8_t c1 = ctl[0] & (uint8_t)~(CTL1_STOP | CTL1_12_24);
    if (c1 != ctl[0])
        reg_write(REG_CONTROL_1, &c1, 1);

    // Control_3: battery switch-over in standard mode with battery low detection (PM = 000).
    // Keep the interrupt enables; BSF is cleared by writing 0, BLF is read-only.
    if (ctl[2] & CTL3_PM_MASK) {
        uint8_t c3 = ctl[2] & 0x03;
        reg_write(REG_CONTROL_3, &c3, 1);
    }
    return true;
}

rtc_status_t rtc_read(int64_t *t) {
    if (!present)
        return RTC_ABSENT;
    uint8_t regs[TIMECONV_RTC_REGS];
    if (!reg_read(REG_SECONDS, regs, sizeof(regs)))
        return RTC_ABSENT;
    if (regs[0] & SECONDS_OS)
        return RTC_NOT_SET;
    int64_t v;
    if (!timeconv_from_rtc(regs, &v))
        return RTC_INVALID;
    *t = v;
    return RTC_OK;
}

bool rtc_write(int64_t t) {
    uint8_t regs[TIMECONV_RTC_REGS];
    if (!present || !timeconv_to_rtc(t, regs))
        return false;
    if (!reg_write(REG_SECONDS, regs, sizeof(regs)))
        return false;
    // The OS flag can't be cleared while the oscillator is stopped, and the seconds may have
    // ticked over since the write.
    int64_t back;
    return rtc_read(&back) == RTC_OK && back >= t && back <= t + 2;
}

bool rtc_battery_low(void) {
    uint8_t c3;
    return present && reg_read(REG_CONTROL_3, &c3, 1) && (c3 & CTL3_BLF);
}

bool rtc_read_offset(int *steps) {
    uint8_t v;
    if (!present || !reg_read(REG_OFFSET, &v, 1) || (v & OFFSET_MODE))
        return false;
    *steps = (v & 0x40) ? (int)v - 128 : (int)v;       // sign-extend 7 bits
    return true;
}

bool rtc_write_offset(int steps) {
    if (!present || steps < TIMECONV_CAL_STEPS_MIN || steps > TIMECONV_CAL_STEPS_MAX)
        return false;
    uint8_t v = (uint8_t)steps & 0x7f;                  // MODE = 0
    int back;
    return reg_write(REG_OFFSET, &v, 1) && rtc_read_offset(&back) && back == steps;
}

const char *rtc_status_str(rtc_status_t st) {
    switch (st) {
    case RTC_OK:        return "ok";
    case RTC_ABSENT:    return "not found";
    case RTC_NOT_SET:   return "not set";
    case RTC_INVALID:   return "invalid";
    }
    return "?";
}
