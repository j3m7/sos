// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Why the chip last reset, decoded once at boot from what the chip and SOS kept: the fault record
// (core/fault.h), the watchdog (its REASON register, and whether it was a real timeout,
// core/wdog.h) and POWMAN's CHIP_RESET register. The boot log, `uptime`, the log files' boot
// marker, the `resets` health check and the reset history (core/history.h) all use this one
// answer (docs/design/health.md §9.5).
//
// resetwhy.c is pure (host tested); resetwhy_board.c reads the registers.

#ifndef SOS_RESETWHY_H
#define SOS_RESETWHY_H

#include <stdbool.h>
#include <stdint.h>

// The values are stored in the history sector: never renumber, only add.
typedef enum {
    RESETWHY_UNKNOWN = 0,       // no register names it
    RESETWHY_POWER_ON = 1,      // power applied (HAD_POR, with or without HAD_BOR)
    RESETWHY_REBOOT = 2,        // asked for: `reboot`, picotool, the boot ROM (the watchdog, forced)
    RESETWHY_RESET_PIN = 3,     // the RUN pin
    RESETWHY_BROWNOUT = 4,      // the core supply fell under the detector's threshold
    RESETWHY_WATCHDOG = 5,      // a real watchdog timeout: the main loop stopped
    RESETWHY_FAULT = 6,         // a hard fault or panic, with fault.c's record
    RESETWHY_GLITCH = 7,        // the glitch detector
    RESETWHY_DEBUGGER = 8,      // a reset asked by a debugger (SWD)
    RESETWHY_COUNT
} resetwhy_t;

// POWMAN CHIP_RESET's bits (RP2350 datasheet §6.4); resetwhy_board.c checks them against the
// SDK's names.
#define RESETWHY_HAD_POR                     (1u << 16)
#define RESETWHY_HAD_BOR                     (1u << 17)
#define RESETWHY_HAD_RUN_LOW                 (1u << 18)
#define RESETWHY_HAD_DP_RESET_REQ            (1u << 19)
#define RESETWHY_HAD_RESCUE                  (1u << 21)
#define RESETWHY_HAD_WATCHDOG_RESET_POWMAN_ASYNC (1u << 22)
#define RESETWHY_HAD_WATCHDOG_RESET_POWMAN   (1u << 23)
#define RESETWHY_HAD_WATCHDOG_RESET_SWCORE   (1u << 24)
#define RESETWHY_HAD_SWCORE_PD               (1u << 25)
#define RESETWHY_HAD_GLITCH_DETECT           (1u << 26)
#define RESETWHY_HAD_HZD_SYS_RESET_REQ       (1u << 27)
#define RESETWHY_HAD_WATCHDOG_RESET_PSM      (1u << 28)

// The reason, from:
//   fault       fault.c found a record (it resets through the watchdog)
//   wd_reason   the watchdog's REASON register (TIMER, FORCE bits); non-zero means this boot came
//               from the watchdog, and then CHIP_RESET still holds the reset before that one
//   wd_timeout  wdog_caused_reset(): the watchdog fired by itself, not asked to
//   chip_reset  POWMAN CHIP_RESET
// In this order: a fault record; the watchdog (a timeout, else a reboot asked for); then
// CHIP_RESET, first the bits of resets that don't reset POWMAN and so may sit beside an older
// bit (glitch detector; the debugger's; the watchdog's own POWMAN modes, which SOS doesn't use);
// then HAD_POR, which is a power-on WHETHER OR NOT HAD_BOR IS SET TOO: on a cold power-up the
// brownout detector trips as the core rail rises, so an ordinary plug-in can leave both. Only
// HAD_BOR without HAD_POR is a brownout (the supply dipped under the threshold without falling
// to nothing). A brownout in the first moments of a power-up thus reads as a power-on, which is
// what it nearly was. Then the RUN pin. Nothing set: unknown.
resetwhy_t resetwhy_decode(bool fault, uint32_t wd_reason, bool wd_timeout, uint32_t chip_reset);

// "power-on", "reboot", "reset pin", "brownout", "watchdog timeout", "fault", "glitch detector",
// "debugger", "unknown"; "?" for a value out of range (a record of newer firmware).
const char *resetwhy_name(resetwhy_t why);

// The reasons worth a WARN in `resets`: everything but power-on, reboot and the reset pin.
bool resetwhy_warns(resetwhy_t why);

// --- the board (resetwhy_board.c) --------------------------------------------------------------

// Once, in main() after fault_boot() and wdog_start(): read the registers and decode.
void resetwhy_boot(void);
resetwhy_t resetwhy(void);
uint32_t resetwhy_chip_reset(void);         // CHIP_RESET as read at boot
uint32_t resetwhy_wd_reason(void);          // the watchdog's REASON as read at boot

// The reason as one phrase: fault.c's record for a fault ("hard fault on core 0 at ..."), else
// resetwhy_name(), with CHIP_RESET for an unknown one.
const char *resetwhy_text(void);

#endif
