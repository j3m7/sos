// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Test-only SD card write faults (see sdfault.h). Compiled only with -DSOS_TEST_COMMANDS=ON.

#ifndef SOS_TEST_COMMANDS
#error "sdfault.c is a test tool and belongs only in test builds (-DSOS_TEST_COMMANDS=ON)"
#endif

#include "files/sdfault.h"

#include <string.h>

static sdfault_state_t st;

bool sdfault_arm(sdfault_mode_t mode, uint32_t n) {
    if ((mode != SDFAULT_FAIL && mode != SDFAULT_REBOOT) || n == 0 || n > SDFAULT_MAX_N)
        return false;
    memset(&st, 0, sizeof(st));
    st.armed = mode;
    st.at = n;
    st.counting = true;
    return true;
}

void sdfault_off(void) {
    memset(&st, 0, sizeof(st));
}

sdfault_action_t sdfault_write(void) {
    if (!st.counting)
        return SDFAULT_PASS;
    if (st.writes != UINT32_MAX)
        st.writes++;
    if (st.armed == SDFAULT_OFF || st.writes != st.at)
        return SDFAULT_PASS;
    st.fired = st.armed;
    st.armed = SDFAULT_OFF;
    return st.fired == SDFAULT_FAIL ? SDFAULT_DO_FAIL : SDFAULT_DO_REBOOT;
}

const sdfault_state_t *sdfault_state(void) {
    return &st;
}
