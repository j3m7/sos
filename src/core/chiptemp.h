// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The chip's temperature, from the RP2350's own sensor through the ADC (datasheet section
// 12.4.6), for `temp`, the log and the glitch detector's reports (issue #92).
//
// Two limits go wherever the number is shown:
// - It is the chip's temperature, not the room's. The chip runs warmer than the air, more so
//   under Wi-Fi load.
// - It is uncalibrated: the sensor's voltage at a given temperature differs from chip to chip,
//   and the conversion assumes the ADC reference is exactly 3.3 V (1% off is about 4 degrees).
//   It is a few degrees off in absolute terms, but good for change over time on one board,
//   which is what the glitch detector test needs.
//
// Temperatures are in tenths of a degree Celsius, as int.
//
// chiptemp.c is pure C (host tested): the conversion, the text, the lowest and highest
// readings, when to log, and the form kept in a POWMAN scratch register. chiptemp_board.c
// reads the ADC.

#ifndef SOS_CHIPTEMP_H
#define SOS_CHIPTEMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CHIPTEMP_NONE INT16_MIN         // no reading

#define CHIPTEMP_VREF_UV  3300000u      // ADC reference on the Pico 2 W: its 3.3 V supply
#define CHIPTEMP_SAMPLES  64            // ADC conversions averaged per reading (2 us each)
#define CHIPTEMP_SAMPLE_S 10            // a reading every 10 s from the main loop

// The periodic log line: one at the first reading after boot, then whenever the reading has
// moved CHIPTEMP_LOG_STEP or more from the last one logged (at most one a minute), and at least
// one an hour. A steady board adds about a line an hour to the log; a warming or cooling one
// shows the change a degree at a time.
#define CHIPTEMP_LOG_STEP  10           // tenths: 1.0 degree
#define CHIPTEMP_LOG_MIN_S 60
#define CHIPTEMP_LOG_MAX_S 3600

// Room for chiptemp_format()'s text, e.g. "-3276.8 C".
#define CHIPTEMP_TEXT_MAX 12

// --- pure ----------------------------------------------------------------------------------------

// Tenths of a degree from the sum of n 12-bit conversions of the sensor, with the ADC reference
// at vref_uv microvolts: T = 27 - (V - 0.706) / 0.001721 (datasheet 12.4.6). Rounded to the
// nearest tenth. CHIPTEMP_NONE if n is 0.
int chiptemp_tenths(uint32_t sum, unsigned n, uint32_t vref_uv);

// "31.2 C", "-0.5 C"; "no reading" for CHIPTEMP_NONE. Always terminated.
void chiptemp_format(char *out, size_t out_size, int tenths);

// How long, for "... ago": "45 s", "12 min", "3 h 05 min", "2 d 04 h". Always terminated.
void chiptemp_format_age(char *out, size_t out_size, uint32_t seconds);

// The readings since boot.
typedef struct {
    unsigned count;                     // readings so far; the rest is meaningless while 0
    int last, min, max;                 // tenths
    uint32_t last_s, min_s, max_s;      // uptime of each, seconds
    bool logged_any;                    // a line has been logged since boot
    int logged;                         // the reading in the last line logged, tenths
    uint32_t logged_s;                  // ... and its uptime
} chiptemp_stats_t;

// Count one reading taken at uptime now_s. CHIPTEMP_NONE is ignored.
void chiptemp_stats_add(chiptemp_stats_t *st, int tenths, uint32_t now_s);

// Should the latest reading be logged now (see CHIPTEMP_LOG_STEP)? If so, the caller logs it and
// calls chiptemp_stats_logged().
bool chiptemp_log_due(const chiptemp_stats_t *st);
void chiptemp_stats_logged(chiptemp_stats_t *st);

// A reading in the form kept in a POWMAN scratch register: a marker in the upper half, so that
// whatever the register held at power-on isn't taken for a reading. chiptemp_unpack() gives
// CHIPTEMP_NONE for a register without the marker, or one packed from CHIPTEMP_NONE.
uint32_t chiptemp_pack(int tenths);
int chiptemp_unpack(uint32_t reg);

// --- the sensor (chiptemp_board.c) ---------------------------------------------------------------

// Early in boot: the ADC and the sensor on, and the first reading.
void chiptemp_init(void);

// From the main loop: a reading every CHIPTEMP_SAMPLE_S, logged when chiptemp_log_due() says.
void chiptemp_poll(void);

// A reading now (it counts in the lowest and highest too).
int chiptemp_read(void);

// The latest reading (at most CHIPTEMP_SAMPLE_S old once the main loop runs), or
// CHIPTEMP_NONE before the first.
int chiptemp_last(void);

// The readings since boot.
const chiptemp_stats_t *chiptemp_stats(void);

#endif
