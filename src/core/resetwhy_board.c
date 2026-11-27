// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See resetwhy.h: the registers, read once at boot.

#include "core/resetwhy.h"

#include <stdio.h>

#include "hardware/regs/powman.h"
#include "hardware/structs/powman.h"
#include "hardware/structs/watchdog.h"

#include "core/fault.h"
#include "core/wdog.h"

_Static_assert(RESETWHY_HAD_POR == POWMAN_CHIP_RESET_HAD_POR_BITS, "CHIP_RESET bits");
_Static_assert(RESETWHY_HAD_BOR == POWMAN_CHIP_RESET_HAD_BOR_BITS, "CHIP_RESET bits");
_Static_assert(RESETWHY_HAD_RUN_LOW == POWMAN_CHIP_RESET_HAD_RUN_LOW_BITS, "CHIP_RESET bits");
_Static_assert(RESETWHY_HAD_DP_RESET_REQ == POWMAN_CHIP_RESET_HAD_DP_RESET_REQ_BITS,
               "CHIP_RESET bits");
_Static_assert(RESETWHY_HAD_RESCUE == POWMAN_CHIP_RESET_HAD_RESCUE_BITS, "CHIP_RESET bits");
_Static_assert(RESETWHY_HAD_WATCHDOG_RESET_POWMAN_ASYNC ==
               POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_POWMAN_ASYNC_BITS, "CHIP_RESET bits");
_Static_assert(RESETWHY_HAD_WATCHDOG_RESET_POWMAN ==
               POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_POWMAN_BITS, "CHIP_RESET bits");
_Static_assert(RESETWHY_HAD_WATCHDOG_RESET_SWCORE ==
               POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_SWCORE_BITS, "CHIP_RESET bits");
_Static_assert(RESETWHY_HAD_SWCORE_PD == POWMAN_CHIP_RESET_HAD_SWCORE_PD_BITS, "CHIP_RESET bits");
_Static_assert(RESETWHY_HAD_GLITCH_DETECT == POWMAN_CHIP_RESET_HAD_GLITCH_DETECT_BITS,
               "CHIP_RESET bits");
_Static_assert(RESETWHY_HAD_HZD_SYS_RESET_REQ == POWMAN_CHIP_RESET_HAD_HZD_SYS_RESET_REQ_BITS,
               "CHIP_RESET bits");
_Static_assert(RESETWHY_HAD_WATCHDOG_RESET_PSM == POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_PSM_BITS,
               "CHIP_RESET bits");

static resetwhy_t why = RESETWHY_UNKNOWN;
static uint32_t chip_reset, wd_reason;

void resetwhy_boot(void) {
    chip_reset = powman_hw->chip_reset;
    wd_reason = watchdog_hw->reason;
    why = resetwhy_decode(fault_last() != NULL, wd_reason, wdog_caused_reset(), chip_reset);
}

resetwhy_t resetwhy(void) {
    return why;
}

uint32_t resetwhy_chip_reset(void) {
    return chip_reset;
}

uint32_t resetwhy_wd_reason(void) {
    return wd_reason;
}

const char *resetwhy_text(void) {
    static char unknown[40];
    if (why == RESETWHY_FAULT && fault_last())
        return fault_last();
    if (why == RESETWHY_UNKNOWN) {
        snprintf(unknown, sizeof(unknown), "unknown (CHIP_RESET 0x%08lx)",
                 (unsigned long)chip_reset);
        return unknown;
    }
    return resetwhy_name(why);
}
