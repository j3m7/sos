// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See resetwhy.h.

#include "core/resetwhy.h"

#include <stddef.h>

resetwhy_t resetwhy_decode(bool fault, uint32_t wd_reason, bool wd_timeout, uint32_t chip_reset) {
    if (fault)
        return RESETWHY_FAULT;
    if (wd_reason)
        return wd_timeout ? RESETWHY_WATCHDOG : RESETWHY_REBOOT;
    // Resets that leave POWMAN alone: an older bit may still be set beside theirs.
    if (chip_reset & RESETWHY_HAD_GLITCH_DETECT)
        return RESETWHY_GLITCH;
    if (chip_reset & (RESETWHY_HAD_HZD_SYS_RESET_REQ | RESETWHY_HAD_DP_RESET_REQ |
                      RESETWHY_HAD_RESCUE))
        return RESETWHY_DEBUGGER;
    if (chip_reset & (RESETWHY_HAD_WATCHDOG_RESET_PSM | RESETWHY_HAD_WATCHDOG_RESET_SWCORE |
                      RESETWHY_HAD_WATCHDOG_RESET_POWMAN |
                      RESETWHY_HAD_WATCHDOG_RESET_POWMAN_ASYNC | RESETWHY_HAD_SWCORE_PD))
        return RESETWHY_WATCHDOG;
    // A cold power-up trips the brownout detector as the core rail rises: POR wins.
    if (chip_reset & RESETWHY_HAD_POR)
        return RESETWHY_POWER_ON;
    if (chip_reset & RESETWHY_HAD_BOR)
        return RESETWHY_BROWNOUT;
    if (chip_reset & RESETWHY_HAD_RUN_LOW)
        return RESETWHY_RESET_PIN;
    return RESETWHY_UNKNOWN;
}

const char *resetwhy_name(resetwhy_t why) {
    static const char *const names[RESETWHY_COUNT] = {
        [RESETWHY_UNKNOWN] = "unknown",
        [RESETWHY_POWER_ON] = "power-on",
        [RESETWHY_REBOOT] = "reboot",
        [RESETWHY_RESET_PIN] = "reset pin",
        [RESETWHY_BROWNOUT] = "brownout",
        [RESETWHY_WATCHDOG] = "watchdog timeout",
        [RESETWHY_FAULT] = "fault",
        [RESETWHY_GLITCH] = "glitch detector",
        [RESETWHY_DEBUGGER] = "debugger",
    };
    return (unsigned)why < RESETWHY_COUNT ? names[why] : "?";
}

bool resetwhy_warns(resetwhy_t why) {
    return why != RESETWHY_POWER_ON && why != RESETWHY_REBOOT && why != RESETWHY_RESET_PIN;
}
