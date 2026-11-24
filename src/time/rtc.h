// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// PCF8523 real-time clock on the PiCowbell Adalogger: I2C0, SDA GP4, SCL GP5, address 0x68,
// with a CR1220 backup cell. Time is kept in UTC, 24-hour mode.
//
// Main loop only: not safe to call from interrupt context (lwIP callbacks included).

#ifndef SOS_RTC_H
#define SOS_RTC_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RTC_OK,             // holds a valid time
    RTC_ABSENT,         // no answer on the I2C bus
    RTC_NOT_SET,        // oscillator stopped since the time was last written (OS flag), e.g.
                        // first power-up or the backup cell ran out while unpowered
    RTC_INVALID,        // registers don't decode to a date in 2000-2099
} rtc_status_t;

// Set up I2C and the chip: battery switch-over on (the chip's power-on default is off, which
// loses the time at every power cut), clock running, 24-hour mode. Returns false if the chip
// doesn't answer.
bool rtc_init(void);

// Read the time (Unix seconds, UTC). *t is only written when the result is RTC_OK.
rtc_status_t rtc_read(int64_t *t);

// Write the time; clears the OS flag. Returns false on an I2C error, a time outside 2000-2099,
// or if the time doesn't read back (e.g. the oscillator isn't running, so OS stays set).
bool rtc_write(int64_t t);

// After rtc_init(): true if SDA and SCL had pull-ups (the Adalogger is attached), whether or not
// the chip answered. Tells "board missing" from "chip not answering".
bool rtc_bus_pulled_up(void);

// True if the chip reports its backup cell below ~2.5 V. False if it's fine or the chip is absent.
bool rtc_battery_low(void);

const char *rtc_status_str(rtc_status_t st);

#endif
