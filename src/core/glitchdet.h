// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The RP2350's glitch detector: four circuits that reset the chip when its clock or core supply
// is disturbed enough for logic to misbehave (datasheet section 10.9).
//
// This file arms it by software, as a setting, and reports what it does. It is armed by default,
// at the highest sensitivity, so that every board running SOS shows over time whether the
// detector ever resets a healthy board. THAT IS A TRIAL AID, NOT PROTECTION:
// - It is armed only once the configuration has loaded, a second or two into boot, so the
//   early boot (where the boot ROM checks signatures) isn't covered.
// - Software armed it, and software can disarm it.
// - The loop guard below disarms it after a few resets in a row, which is exactly what someone
//   glitching the board on purpose would cause.
// A secured board gets the detector from the OTP flag CRIT1.GLITCH_DETECTOR_ENABLE instead:
// armed from reset, with no off switch and no guard. The purpose here is to find out, in
// normal use, whether the detector resets a healthy board, before that bit is burned
// (docs/securing-a-board.md, decision 5). A DETECTOR RESET NOBODY FORCED IS THEREFORE A FINDING:
// see "Glitch detector resets" in docs/development.md.
//
// What it keeps across a detector reset is in the POWMAN scratch registers 0 to 3, which that
// reset doesn't clear (it does clear the watchdog's, and with them the fault record):
// the count of detector resets since power-on, the count of them in a row soon after boot, the
// uptime, so the next boot can say how long the board had been up, and the latest chip
// temperature and supply voltage (core/sensors.h), so it can say how warm the chip was and
// whether the supply had sagged (issue #92).
//
// The loop guard: if the detector resets the board GLITCHDET_GUARD_RESETS times in a row, each
// before the board had been up GLITCHDET_STABLE_S seconds, the next boot doesn't arm it and
// says so. Otherwise a detector that fires during boot would reset the board for ever, with no
// way to reach the command that turns it off.

#ifndef SOS_GLITCHDET_H
#define SOS_GLITCHDET_H

#include <stdbool.h>
#include <stdint.h>

#define GLITCHDET_GUARD_RESETS 3
#define GLITCHDET_STABLE_S     180

// First thing in main(): note why the chip last reset and for how long it had been up.
void glitchdet_boot(void);

// Once the configuration is loaded and the log works: say what the last reset was, and arm the
// detector if `setting` asks for it and the guard allows. The setting is what
// config_glitch_mode() makes of sos_config_t's:
// 0 = off, 1..4 = armed at sensitivity 0..3, 5..8 = watching at sensitivity 0..3.
void glitchdet_start(unsigned setting);

// From the main loop: keeps the uptime and temperature records, and clears the guard's count
// once the board has been up long enough.
void glitchdet_poll(void);

// Arm or disarm now (the `glitch` command, after the setting changed). Returns false if arming
// didn't take (the registers read back differently).
bool glitchdet_apply(unsigned setting);

// Was the last reset the detector's? Which detectors had fired (bits 0-3), how long the
// board had been up (seconds; 0 if unknown), and the last chip temperature and supply readings
// before the reset, taken every SENSORS_SAMPLE_S (tenths of a degree, millivolts;
// SENSORS_NONE if none was kept).
bool glitchdet_caused_reset(void);
uint32_t glitchdet_last_triggers(void);
uint32_t glitchdet_last_uptime_s(void);
int glitchdet_last_temp(void);
int glitchdet_last_vsys(void);

// Detector resets since power-on.
uint32_t glitchdet_resets(void);

// -1 if not armed now, else the sensitivity 0..3 it is armed at.
int glitchdet_armed_sensitivity(void);

// Watching: the detectors run at a sensitivity without being armed. A trigger is recorded and
// logged but resets nothing, so there is no lost fault record and no need for the guard. -1 if
// not watching, else the sensitivity; which detectors have triggered since boot (bits 0-3; a
// detector may stay triggered until the next reset, so each is reported once per boot); and the
// uptime of the first, with the chip temperature and supply then (SENSORS_NONE before any).
int glitchdet_watching_sensitivity(void);
uint32_t glitchdet_watch_triggers(void);
uint32_t glitchdet_watch_first_s(void);
int glitchdet_watch_first_temp(void);
int glitchdet_watch_first_vsys(void);

// The guard stopped it being armed at this boot.
bool glitchdet_guard_tripped(void);

// Make one detector fire (to check that a detector reset is seen and reported). If the
// detector is armed this resets the chip at once.
void glitchdet_simulate(void);

#endif
