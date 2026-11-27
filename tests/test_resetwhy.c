// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/core/resetwhy.c: the last reset's reason from the fault record, the
// watchdog and POWMAN CHIP_RESET (docs/design/health.md §9.5).

#include "core/resetwhy.h"
#include "test.h"

#define TIMER 1u                        // the watchdog's REASON bits
#define FORCE 2u

TEST(fault_first) {
    CHECK_EQ(resetwhy_decode(true, TIMER, true, RESETWHY_HAD_BOR), RESETWHY_FAULT);
    CHECK_EQ(resetwhy_decode(true, 0, false, 0), RESETWHY_FAULT);
}

TEST(watchdog_before_chip_reset) {
    // A reboot or a timeout leaves CHIP_RESET as the reset before it left it.
    CHECK_EQ(resetwhy_decode(false, TIMER, true, RESETWHY_HAD_GLITCH_DETECT), RESETWHY_WATCHDOG);
    CHECK_EQ(resetwhy_decode(false, TIMER, false, RESETWHY_HAD_GLITCH_DETECT), RESETWHY_REBOOT);
    CHECK_EQ(resetwhy_decode(false, FORCE, false, RESETWHY_HAD_POR), RESETWHY_REBOOT);
    CHECK_EQ(resetwhy_decode(false, TIMER, false, RESETWHY_HAD_BOR), RESETWHY_REBOOT);
}

TEST(each_bit_alone) {
    CHECK_EQ(resetwhy_decode(false, 0, false, RESETWHY_HAD_POR), RESETWHY_POWER_ON);
    CHECK_EQ(resetwhy_decode(false, 0, false, RESETWHY_HAD_BOR), RESETWHY_BROWNOUT);
    CHECK_EQ(resetwhy_decode(false, 0, false, RESETWHY_HAD_RUN_LOW), RESETWHY_RESET_PIN);
    CHECK_EQ(resetwhy_decode(false, 0, false, RESETWHY_HAD_DP_RESET_REQ), RESETWHY_DEBUGGER);
    CHECK_EQ(resetwhy_decode(false, 0, false, RESETWHY_HAD_RESCUE), RESETWHY_DEBUGGER);
    CHECK_EQ(resetwhy_decode(false, 0, false, RESETWHY_HAD_HZD_SYS_RESET_REQ), RESETWHY_DEBUGGER);
    CHECK_EQ(resetwhy_decode(false, 0, false, RESETWHY_HAD_GLITCH_DETECT), RESETWHY_GLITCH);
    CHECK_EQ(resetwhy_decode(false, 0, false, RESETWHY_HAD_WATCHDOG_RESET_PSM), RESETWHY_WATCHDOG);
    CHECK_EQ(resetwhy_decode(false, 0, false, RESETWHY_HAD_WATCHDOG_RESET_SWCORE),
             RESETWHY_WATCHDOG);
    CHECK_EQ(resetwhy_decode(false, 0, false, RESETWHY_HAD_WATCHDOG_RESET_POWMAN),
             RESETWHY_WATCHDOG);
    CHECK_EQ(resetwhy_decode(false, 0, false, RESETWHY_HAD_WATCHDOG_RESET_POWMAN_ASYNC),
             RESETWHY_WATCHDOG);
    CHECK_EQ(resetwhy_decode(false, 0, false, RESETWHY_HAD_SWCORE_PD), RESETWHY_WATCHDOG);
    CHECK_EQ(resetwhy_decode(false, 0, false, 0), RESETWHY_UNKNOWN);
    CHECK_EQ(resetwhy_decode(false, 0, false, 1u), RESETWHY_UNKNOWN);   // DOUBLE_TAP only
}

TEST(bor_with_por_is_power_on) {
    // A cold power-up trips the brownout detector as the core rail rises (Reviewer, #255).
    CHECK_EQ(resetwhy_decode(false, 0, false, RESETWHY_HAD_POR | RESETWHY_HAD_BOR),
             RESETWHY_POWER_ON);
    CHECK_EQ(resetwhy_decode(false, 0, false, RESETWHY_HAD_BOR | RESETWHY_HAD_RUN_LOW),
             RESETWHY_BROWNOUT);
}

TEST(stale_por_beside_glitch) {
    // A glitch detector reset doesn't reset POWMAN: the power-on's bit is still there.
    CHECK_EQ(resetwhy_decode(false, 0, false, RESETWHY_HAD_POR | RESETWHY_HAD_GLITCH_DETECT),
             RESETWHY_GLITCH);
    CHECK_EQ(resetwhy_decode(false, 0, false, RESETWHY_HAD_POR | RESETWHY_HAD_DP_RESET_REQ),
             RESETWHY_DEBUGGER);
}

TEST(names_and_warns) {
    CHECK(!strcmp(resetwhy_name(RESETWHY_BROWNOUT), "brownout"));
    CHECK(!strcmp(resetwhy_name(RESETWHY_POWER_ON), "power-on"));
    CHECK(!strcmp(resetwhy_name(RESETWHY_WATCHDOG), "watchdog timeout"));
    CHECK(!strcmp(resetwhy_name((resetwhy_t)200), "?"));
    for (int w = 0; w < RESETWHY_COUNT; w++)
        CHECK(resetwhy_name((resetwhy_t)w) != NULL);
    CHECK(!resetwhy_warns(RESETWHY_POWER_ON));
    CHECK(!resetwhy_warns(RESETWHY_REBOOT));
    CHECK(!resetwhy_warns(RESETWHY_RESET_PIN));
    CHECK(resetwhy_warns(RESETWHY_BROWNOUT));
    CHECK(resetwhy_warns(RESETWHY_FAULT));
    CHECK(resetwhy_warns(RESETWHY_WATCHDOG));
    CHECK(resetwhy_warns(RESETWHY_GLITCH));
    CHECK(resetwhy_warns(RESETWHY_DEBUGGER));
    CHECK(resetwhy_warns(RESETWHY_UNKNOWN));
}

TEST_MAIN(
    RUN(fault_first);
    RUN(watchdog_before_chip_reset);
    RUN(each_bit_alone);
    RUN(bor_with_por_is_power_on);
    RUN(stale_por_beside_glitch);
    RUN(names_and_warns);
)
