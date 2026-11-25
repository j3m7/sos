// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The RP2350's glitch detector: four circuits that reset the chip when its clock or core supply
// is disturbed enough for logic to misbehave (datasheet section 10.9). On a secured board it
// would be armed for good by an OTP bit. This file does two things that need no OTP:
//
// - Reporting, in every build: whether the last reset was the detector's, which detectors
//   fired, and how many such resets there have been since power-on.
// - The soak test, only in a build made with the CMake setting SOS_GLITCH_SOAK=<0..3>: arm the
//   detector by software at that sensitivity as early as possible at boot, so the board can
//   run its normal work for days and show whether the detector ever resets a healthy board
//   (docs/securing-a-board.md, decision 5). A normal build never arms it.

#ifndef SOS_GLITCHDET_H
#define SOS_GLITCHDET_H

#include <stdbool.h>
#include <stdint.h>

// First thing in main(): note why the chip last reset, and in a soak build arm the detector.
void glitchdet_boot(void);

// Once the log works: say what glitchdet_boot() found and did.
void glitchdet_report(void);

// Was the last reset the detector's?
bool glitchdet_caused_reset(void);

// Detector resets since power-on (kept in a register the detector's reset doesn't clear).
uint32_t glitchdet_resets(void);

// Which detectors fired before the last reset (bits 0-3), as read at boot.
uint32_t glitchdet_last_triggers(void);

// -1 if the detector isn't armed by this build, else the sensitivity 0..3 it was armed at.
int glitchdet_armed_sensitivity(void);

// Soak builds only: make one detector fire, to check that a detector reset is seen and
// reported. Resets the chip at once. Does nothing in a normal build.
void glitchdet_simulate(void);

#endif
