// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "core/glitchdet.h"

#include "hardware/structs/glitch_detector.h"
#include "hardware/structs/powman.h"
#include "hardware/regs/powman.h"
#include "pico/stdlib.h"

#include "core/log.h"

// The count of detector resets lives in a POWMAN scratch register: the detector's reset clears
// the watchdog's scratch registers (where the fault record is) but not POWMAN's. The upper half
// is a marker, so whatever else may be in the register isn't taken for a count.
#define COUNT_REG    (powman_hw->scratch[0])
#define COUNT_MAGIC  0x474c0000u        // "GL"
#define COUNT_MASK   0x0000ffffu

// SENSITIVITY register: 0xde in the top byte means "use this register, not the OTP default";
// each detector's two bits must be accompanied by their inverse.
#define SENS_USE_REGISTER (0xdeu << 24)

static bool caused_reset;
static uint32_t triggers;
static int armed = -1;

static uint32_t count(void) {
    uint32_t v = COUNT_REG;
    return (v & ~COUNT_MASK) == COUNT_MAGIC ? (v & COUNT_MASK) : 0;
}

void glitchdet_boot(void) {
    caused_reset = (powman_hw->chip_reset & POWMAN_CHIP_RESET_HAD_GLITCH_DETECT_BITS) != 0;
    triggers = glitch_detector_hw->trig_status & 0xf;
    if (caused_reset)
        COUNT_REG = COUNT_MAGIC | ((count() + 1) & COUNT_MASK);
    else if ((COUNT_REG & ~COUNT_MASK) != COUNT_MAGIC)
        COUNT_REG = COUNT_MAGIC;        // power-on, or the first boot of this firmware: zero

#ifdef SOS_GLITCH_SOAK
    uint32_t s = (uint32_t)(SOS_GLITCH_SOAK) & 3u, field = 0;
    for (unsigned det = 0; det < 4; det++)
        field |= s << (2 * det) | (~s & 3u) << (8 + 2 * det);
    glitch_detector_hw->sensitivity = SENS_USE_REGISTER | field;
    glitch_detector_hw->arm = 0;        // anything but the "no" pattern arms
    armed = (int)s;
#endif
}

void glitchdet_report(void) {
    if (caused_reset)
        log_msg(SOS_LOG_ERR, "last reset: glitch detector (detectors 0x%x fired; %lu such reset%s "
                             "since power-on)", (unsigned)triggers, (unsigned long)count(),
                count() == 1 ? "" : "s");
    if (armed >= 0)
        log_msg(SOS_LOG_NOTICE, "glitch detector: armed by software at sensitivity %d (soak test "
                                "build; not a normal build)", armed);
}

bool glitchdet_caused_reset(void) {
    return caused_reset;
}

uint32_t glitchdet_resets(void) {
    return count();
}

uint32_t glitchdet_last_triggers(void) {
    return triggers;
}

int glitchdet_armed_sensitivity(void) {
    return armed;
}

void glitchdet_simulate(void) {
#ifdef SOS_GLITCH_SOAK
    glitch_detector_hw->trig_force = 1; // detector 0; armed, so this resets the chip
#endif
}
