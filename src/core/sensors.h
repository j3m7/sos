// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The board's own readings, for `temp`, `power`, the log and the glitch detector's reports
// (issue #92):
// - the chip's temperature, from the RP2350's sensor through the ADC (datasheet 12.4.6);
// - the supply voltage, VSYS, through the board's divider on ADC 3 (Pico 2 W datasheet);
// - whether USB power is present, from the Wi-Fi chip's VBUS sense pin (WL_GPIO2).
//
// The limits go wherever the numbers are shown:
// - The temperature is the chip's, not the room's: the chip runs warmer than the air, more so
//   under Wi-Fi load. It is uncalibrated: the sensor differs from chip to chip, and the
//   conversion assumes the ADC reference is exactly 3.3 V (1% off is about 4 degrees). A few
//   degrees off in absolute terms, but good for change over time on one board.
// - The supply is VSYS, the board's input: USB power through a diode (about 0.35 V below the
//   USB voltage), or what feeds the VSYS pin, such as a battery. It is measured through a 1%
//   divider against the same 3.3 V reference, so it is within a few percent. Each reading is
//   about 130 us of conversions, so a reading taken during a Wi-Fi transmission shows the dip
//   it causes (about 0.13 V on USB on dev-1).
//
// Temperatures are in tenths of a degree Celsius, voltages in millivolts, both as int.
//
// sensors.c is pure C (host tested): the conversions, the text, the lowest and highest
// readings, when to log, and the form kept in a POWMAN scratch register. sensors_board.c reads
// the hardware.

#ifndef SOS_SENSORS_H
#define SOS_SENSORS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SENSORS_NONE INT16_MIN          // no reading

#define SENSORS_VREF_UV  3300000u       // ADC reference on the Pico 2 W: its 3.3 V supply
#define SENSORS_SAMPLES  64             // ADC conversions averaged per reading (2 us each)
#define SENSORS_SAMPLE_S 10             // a reading of each every 10 s from the main loop
// Wait after switching the temperature sensor on before its first reading. Measured on dev-1
// (RP2350 A2): 30.4 degrees at once, 28.9 after 100 us, 26.8 after 1 ms, steady from 2 ms.
#define SENSORS_TEMP_SETTLE_US 10000
// Conversions thrown away after switching the ADC to the VSYS pin, which the Wi-Fi chip's SPI
// clock was driving. Measured on dev-1 (raw, settled 1885): the first 1600 to 1820, the second
// 1874 to 1882, the third and later 1884 to 1886.
#define SENSORS_VSYS_DISCARD 4
// VSYS divider on the Pico 2 W: 20k over 10k (R5, R6), so the pin sees a third of VSYS.
#define SENSORS_VSYS_DIVIDER 3

// The periodic log line, with both readings: one at the first reading after boot; then
// whenever the temperature has moved SENSORS_LOG_STEP_TEMP or the supply SENSORS_LOG_STEP_MV
// from the last line (at most one a minute); at once when USB power comes or goes; and at
// least one an hour. A steady board adds about a line an hour to the log. The supply's step is
// above the dips a Wi-Fi transmission causes on USB power, so traffic alone doesn't log.
#define SENSORS_LOG_STEP_TEMP 10        // tenths: 1.0 degree
#define SENSORS_LOG_STEP_MV   200       // 0.2 V
#define SENSORS_LOG_MIN_S     60
#define SENSORS_LOG_MAX_S     3600

// Room for the text of sensors_format_temp() and sensors_format_mv(), or "no reading". Real
// readings need 10 ("-1480.3 C"); this holds any int, so the compiler can see nothing is cut.
#define SENSORS_TEXT_MAX 16

// --- pure ----------------------------------------------------------------------------------------

// Tenths of a degree from the sum of n 12-bit conversions of the temperature sensor, with the
// ADC reference at vref_uv microvolts: T = 27 - (V - 0.706) / 0.001721 (datasheet 12.4.6).
// Rounded to the nearest tenth. SENSORS_NONE if n is 0.
int sensors_temp_tenths(uint32_t sum, unsigned n, uint32_t vref_uv);

// VSYS in millivolts from the sum of n 12-bit conversions of the VSYS pin: V * divider, rounded
// to the nearest millivolt. SENSORS_NONE if n is 0.
int sensors_vsys_mv(uint32_t sum, unsigned n, uint32_t vref_uv, unsigned divider);

// "31.2 C", "-0.5 C"; "4.56 V" (to 10 mV). "no reading" for SENSORS_NONE. Always terminated.
void sensors_format_temp(char *out, size_t out_size, int tenths);
void sensors_format_mv(char *out, size_t out_size, int mv);

// Both readings as the glitch detector's reports show them: "chip 26.7 C, supply 4.56 V";
// "chip 26.7 C" without a supply reading; "no readings" without either. Always terminated.
#define SENSORS_BOTH_MAX 48
void sensors_format_both(char *out, size_t out_size, int tenths, int mv);

// How long, for "... ago": "45 s", "12 min", "3 h 05 min", "2 d 04 h". Always terminated.
void sensors_format_age(char *out, size_t out_size, uint32_t seconds);

// One reading's values since boot.
typedef struct {
    unsigned count;                     // readings so far; the rest is meaningless while 0
    int last, min, max;
    uint32_t last_s, min_s, max_s;      // uptime of each, seconds
} sensors_stats_t;

// Count one reading taken at uptime now_s. SENSORS_NONE is ignored.
void sensors_stats_add(sensors_stats_t *st, int value, uint32_t now_s);

// The lowest reading in a window that its reader restarts (the `supply` health check: the lowest
// VSYS since its last run). *min starts as SENSORS_NONE and keeps the lower of itself and value;
// SENSORS_NONE is ignored.
void sensors_window_add(int *min, int value);

// Move every value by delta (the temperature's calibration changed), keeping the times.
void sensors_stats_shift(sensors_stats_t *st, int delta);

// A temperature as typed for `temp cal`: "23", "23.5", "-4.2", "+31.0"; at most one decimal,
// SENSORS_CAL_MIN..SENSORS_CAL_MAX. False for anything else.
#define SENSORS_CAL_MIN (-400)          // tenths: -40.0 degrees
#define SENSORS_CAL_MAX 1250            // 125.0, the chip's rated range
bool sensors_parse_temp(const char *s, int *tenths);

// What the last log line said. usb: 1 present, 0 not, -1 unknown.
typedef struct {
    bool any;                           // a line has been logged since boot
    uint32_t s;                         // its uptime
    int temp, mv, usb;
} sensors_logged_t;

// Should a line be logged now, with these readings (see SENSORS_LOG_STEP_TEMP)? If so, the
// caller logs it and calls sensors_log_mark() with the same values. Nothing is due without a
// temperature.
bool sensors_log_due(const sensors_logged_t *lg, int temp, int mv, int usb, uint32_t now_s);
void sensors_log_mark(sensors_logged_t *lg, int temp, int mv, int usb, uint32_t now_s);

// A reading in the form kept in a POWMAN scratch register: `marker` (16 bits) in the upper
// half, so that whatever the register held at power-on isn't taken for a reading.
// sensors_unpack() gives SENSORS_NONE for a register without the marker, or one packed from
// SENSORS_NONE. Values outside 16 bits are kept at the ends of the range.
#define SENSORS_MARK_TEMP 0x4354u       // "CT"
#define SENSORS_MARK_VSYS 0x5356u       // "SV"
uint32_t sensors_pack(int value, uint32_t marker);
int sensors_unpack(uint32_t reg, uint32_t marker);

// --- the hardware (sensors_board.c) --------------------------------------------------------------

// The temperature readings below include the calibration (`temp cal`): the configuration's
// temp_offset, tenths of a degree, 0 = uncalibrated. So do the log line and the glitch
// detector's reports, which take them from here.

// Early in boot, after the configuration has loaded: the ADC and the temperature sensor on, and a first temperature reading for
// sensors_temp_last(). That one doesn't count in the lowest and highest: it's taken before
// Wi-Fi starts, which shifts the readings (sensors_board.c). VSYS can be read only once the
// Wi-Fi chip is running (its SPI clock shares the pin), so the main loop takes the first.
void sensors_init(void);

// From the main loop: a reading of each every SENSORS_SAMPLE_S, logged when sensors_log_due()
// says. Needs the Wi-Fi chip running (cyw43_arch_init done).
void sensors_poll(void);

// A reading now (it counts in the lowest and highest too). The VSYS reading takes the Wi-Fi
// driver's lock for about 0.2 ms; SENSORS_NONE on a board without a VSYS pin.
int sensors_read_temp(void);
int sensors_read_vsys(void);

// The latest readings (at most SENSORS_SAMPLE_S old once the main loop runs), or SENSORS_NONE
// before the first. The temperature has sensors_init()'s before the main loop's first.
int sensors_last_temp(void);
int sensors_last_vsys(void);

// USB power at the latest reading: 1 present, 0 not, -1 not known yet.
int sensors_usb_power(void);

// For `temp cal`: a temperature reading now without the calibration, not counted anywhere.
int sensors_read_temp_uncalibrated(void);

// After the configuration's temp_offset changed from old_offset: move the lowest, highest and
// latest temperatures (and the log's last) by the difference, so they stay comparable.
void sensors_temp_offset_changed(int old_offset);

// The lowest VSYS reading since the last call (SENSORS_NONE if none), and a new window starts.
// For the `supply` health check: a dip between two of its runs is not missed.
int sensors_vsys_min_take(void);

// The readings since boot.
const sensors_stats_t *sensors_temp_stats(void);
const sensors_stats_t *sensors_vsys_stats(void);

#endif
