// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Test-only SD card write and read faults (see sdfault.h). Only with -DSOS_TEST_COMMANDS=ON.

#ifndef SOS_TEST_COMMANDS
#error "sdfault.c is a test tool and belongs only in test builds (-DSOS_TEST_COMMANDS=ON)"
#endif

#include "files/sdfault.h"

#include <string.h>

static sdfault_state_t st;
static struct { uint32_t at, reads; } rd;     // the read fault: at 0 when none is armed

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
    memset(&rd, 0, sizeof(rd));
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

bool sdfault_arm_read(uint32_t n) {
    if (n == 0 || n > SDFAULT_MAX_N)
        return false;
    rd.at = n;
    rd.reads = 0;
    return true;
}

bool sdfault_read(void) {
    if (!rd.at)
        return false;
    if (++rd.reads != rd.at)
        return false;
    rd.at = 0;
    return true;
}

void sdfault_read_state(uint32_t *at, uint32_t *reads) {
    *at = rd.at;
    *reads = rd.reads;
}
