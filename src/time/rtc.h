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

// The crystal load capacitance (Control_1's CAP_SEL), in tenths of a pF.
#define RTC_LOAD_7PF    70          // CAP_SEL = 0, the chip's reset default
#define RTC_LOAD_12_5PF 125         // CAP_SEL = 1: the PiCowbell Adalogger's crystal

// Set up I2C and the chip: battery switch-over on (the chip's power-on default is off, which
// loses the time at every power cut), clock running, 24-hour mode, the crystal load the build
// names (SOS_RTC_LOAD_PF). Returns false if the chip doesn't answer.
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

// The offset register (0Eh), which trims the RTC's rate: see TIMECONV_CAL_* in timeconv.h for
// its steps. `steps` is -64..63; the chip's correction mode is left at "every two hours", its
// default and the one that uses least power. Reading fails if the chip is absent or was set to
// the other mode by something else; writing checks that the value reads back.
bool rtc_read_offset(int *steps);
bool rtc_write_offset(int steps);

// The crystal load (RTC_LOAD_*) in force, as read back from the chip; the one rtc_init() found
// before setting it; the one the build names. The first two are 0 if the chip is absent.
uint8_t rtc_load(void);
uint8_t rtc_load_found(void);
uint8_t rtc_load_board(void);

#ifdef SOS_TEST_COMMANDS
// `time load`: set CAP_SEL to `load` (RTC_LOAD_*) until the next boot sets the build's again.
// False if the chip is absent or the value doesn't read back.
bool rtc_set_load(uint8_t load);
#endif

const char *rtc_status_str(rtc_status_t st);

#endif
