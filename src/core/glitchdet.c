// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "core/glitchdet.h"

#include "hardware/structs/glitch_detector.h"
#include "hardware/structs/powman.h"
#include "hardware/regs/powman.h"
#include "hardware/structs/watchdog.h"
#include "pico/stdlib.h"

#include "core/log.h"

// POWMAN scratch registers 0 to 2 (general purpose; the boot ROM uses POWMAN's BOOT0-3, not
// these). The upper half of each is a marker, so that whatever was there at power-on isn't
// taken for a value.
#define REG_COUNT   (powman_hw->scratch[0])     // detector resets since power-on
#define REG_STREAK  (powman_hw->scratch[1])     // ... in a row, each soon after boot
#define REG_UPTIME  (powman_hw->scratch[2])     // seconds up, written about once a second
#define MAGIC_COUNT  0x474c0000u                // "GL"
#define MAGIC_STREAK 0x47530000u                // "GS"
#define MAGIC_UPTIME 0x47000000u                // "G", 24 bits of seconds
#define LOW16        0x0000ffffu
#define LOW24        0x00ffffffu

// SENSITIVITY register: 0xde in the top byte means "use this register, not the OTP default";
// each detector's two bits must come with their inverse or the default is used.
#define SENS_USE_REGISTER (0xdeu << 24)
#define ARM_NO            0x5badu               // the register's reset value: not forced
#define DISARM_YES        0xdcafu

static bool caused_reset, guard_tripped, stable_marked;
static uint32_t triggers, last_uptime;
static int armed = -1, watching = -1;
static uint32_t watch_seen;             // detectors seen triggering while watching, this boot
static uint32_t watch_first_s;          // uptime of the first of them
static uint64_t next_uptime_us;

static uint32_t get16(uint32_t reg, uint32_t magic) {
    return (reg & ~LOW16) == magic ? (reg & LOW16) : 0;
}

void glitchdet_boot(void) {
    // The chip's "last reset was a glitch detect" flag, and not a watchdog reset: the flag may
    // still be set after a later reboot or flash (those go through the watchdog, whose reason
    // register the detector's own reset clears), and such a reboot must not count as another
    // detector reset.
    caused_reset = (powman_hw->chip_reset & POWMAN_CHIP_RESET_HAD_GLITCH_DETECT_BITS) != 0 &&
                   watchdog_hw->reason == 0;
    triggers = glitch_detector_hw->trig_status & 0xf;
    glitch_detector_hw->trig_status = 0xf;      // write 1 to clear: don't carry them into this run
    uint32_t count = get16(REG_COUNT, MAGIC_COUNT), streak = get16(REG_STREAK, MAGIC_STREAK);
    if (caused_reset) {
        if ((REG_UPTIME & ~LOW24) == MAGIC_UPTIME)
            last_uptime = REG_UPTIME & LOW24;
        count = (count + 1) & LOW16;
        // In a row: the run before this one ended in a detector reset before it was stable
        // (a stable run clears the streak, see glitchdet_poll).
        streak = (streak + 1) & LOW16;
    } else {
        streak = 0;                     // any other reset (power, watchdog, reboot) ends the row
    }
    REG_COUNT = MAGIC_COUNT | count;
    REG_STREAK = MAGIC_STREAK | streak;
    REG_UPTIME = MAGIC_UPTIME;
}

static void disarm(void) {
    glitch_detector_hw->arm = ARM_NO;
    glitch_detector_hw->disarm = DISARM_YES;
}

bool glitchdet_apply(unsigned setting) {
    bool arm = setting >= 1 && setting <= 4, watch = setting >= 5 && setting <= 8;
    armed = watching = -1;
    watch_seen = 0;
    if (!arm && !watch) {
        disarm();
        glitch_detector_hw->sensitivity = 0;
        return true;
    }
    uint32_t s = (setting - 1) & 3u, field = 0;
    for (unsigned det = 0; det < 4; det++)
        field |= s << (2 * det) | (~s & 3u) << (8 + 2 * det);
    if (watch)
        disarm();                       // before the detectors get more sensitive
    glitch_detector_hw->sensitivity = SENS_USE_REGISTER | field;
    glitch_detector_hw->trig_status = 0xf;
    bool ok = glitch_detector_hw->sensitivity == (SENS_USE_REGISTER | field);
    if (arm) {
        glitch_detector_hw->disarm = 0;
        glitch_detector_hw->arm = 0;    // anything but ARM_NO arms
        ok = ok && (glitch_detector_hw->arm & LOW16) != ARM_NO;
        armed = ok ? (int)s : -1;
    } else {
        watching = ok ? (int)s : -1;
    }
    return ok;
}

void glitchdet_start(unsigned setting) {
    if (caused_reset)
        log_msg(SOS_LOG_KERN | SOS_LOG_ERR,
                "last reset: glitch detector (detectors 0x%x fired, after %lu s up; "
                             "%lu such reset%s since power-on). The fault record doesn't survive "
                             "that reset",
                (unsigned)triggers, (unsigned long)last_uptime, (unsigned long)glitchdet_resets(),
                glitchdet_resets() == 1 ? "" : "s");
    if (setting == 0 || setting > 8)
        return;
    if (setting >= 5) {                 // watching resets nothing, so it needs no guard
        if (glitchdet_apply(setting))
            log_msg(SOS_LOG_KERN | SOS_LOG_NOTICE,
                    "glitch detector: watching at sensitivity %d, not armed (a "
                                    "trigger is logged, the board isn't reset)", watching);
        else
            log_msg(SOS_LOG_KERN | SOS_LOG_ERR,
                    "glitch detector: setting sensitivity %u didn't take",
                    (setting - 1) & 3u);
        return;
    }
    if (get16(REG_STREAK, MAGIC_STREAK) >= GLITCHDET_GUARD_RESETS) {
        guard_tripped = true;
        log_msg(SOS_LOG_KERN | SOS_LOG_ERR,
                "glitch detector: NOT armed at this boot: it reset the board %u "
                             "times in a row within %u s of starting. The setting is unchanged "
                             "(`glitch off` turns it off; a reboot tries again)",
                GLITCHDET_GUARD_RESETS, GLITCHDET_STABLE_S);
        REG_STREAK = MAGIC_STREAK;      // so the next boot does try again
        return;
    }
    if (glitchdet_apply(setting))
        log_msg(SOS_LOG_KERN | SOS_LOG_NOTICE,
                "glitch detector: armed by software at sensitivity %d (a trial "
                                "setting, not protection; sensitivity register 0x%08lx)",
                armed, (unsigned long)glitch_detector_hw->sensitivity);
    else
        log_msg(SOS_LOG_KERN | SOS_LOG_ERR,
                "glitch detector: arming at sensitivity %u didn't take", setting - 1);
}

void glitchdet_poll(void) {
    uint64_t now = time_us_64();
    if (now < next_uptime_us)
        return;
    next_uptime_us = now + 1000000;
    uint32_t up = (uint32_t)(now / 1000000);
    REG_UPTIME = MAGIC_UPTIME | (up & LOW24);
    if (!stable_marked && up >= GLITCHDET_STABLE_S) {
        stable_marked = true;
        REG_STREAK = MAGIC_STREAK;      // up long enough: the next detector reset starts a new row
    }
    if (watching >= 0) {
        // A detector that has fired may stay set until the chip resets, so each detector is
        // reported once per boot: "it would have reset the board at this point".
        uint32_t now_set = glitch_detector_hw->trig_status & 0xf, fresh = now_set & ~watch_seen;
        if (fresh) {
            if (!watch_seen)
                watch_first_s = up;
            watch_seen |= fresh;
            log_msg(SOS_LOG_KERN | SOS_LOG_WARNING,
                    "glitch detector (watching, sensitivity %d): detectors 0x%x "
                                     "triggered after %lu s up; armed, this would have reset the "
                                     "board", watching, (unsigned)fresh, (unsigned long)up);
            glitch_detector_hw->trig_status = fresh;
        }
    }
}

bool glitchdet_caused_reset(void) { return caused_reset; }
uint32_t glitchdet_last_triggers(void) { return triggers; }
uint32_t glitchdet_last_uptime_s(void) { return last_uptime; }
uint32_t glitchdet_resets(void) { return get16(REG_COUNT, MAGIC_COUNT); }
int glitchdet_armed_sensitivity(void) { return armed; }
int glitchdet_watching_sensitivity(void) { return watching; }
uint32_t glitchdet_watch_triggers(void) { return watch_seen; }
uint32_t glitchdet_watch_first_s(void) { return watch_first_s; }
bool glitchdet_guard_tripped(void) { return guard_tripped; }

void glitchdet_simulate(void) {
    glitch_detector_hw->trig_force = 1; // detector 0
}
