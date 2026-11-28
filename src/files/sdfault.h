// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#ifndef SOS_SDFAULT_H
#define SOS_SDFAULT_H

#include <stdbool.h>
#include <stdint.h>

// Test builds only (-DSOS_TEST_COMMANDS=ON): faults injected into SD card writes, for testing
// repair on a board (`sd fault`, docs/design/sd-check.md §10.4), and into reads, for the
// `sd-bus` health check (docs/design/health.md §9.9). This file counts and decides;
// files/sdcard.c asks it once per write (and read) and does the failing or the reset. Pure C, so
// the host tests run it.
//
// The count: one per write request that reaches the card driver (write_blocks_fed() in
// sdcard.c). That is one FatFs disk_write on a plain card, one chunk of up to 8 sectors on an
// encrypted card (the layer encrypts and writes a chunk at a time), and one sector for each
// header write. Repair writes a sector at a time, so for repair it counts sectors. Write 1 is
// the first write after arming.

typedef enum {
    SDFAULT_OFF,
    SDFAULT_FAIL,               // write N returns an error and isn't written
    SDFAULT_REBOOT,             // the board resets before write N reaches the card
} sdfault_mode_t;

typedef enum {
    SDFAULT_PASS,               // write it
    SDFAULT_DO_FAIL,            // don't write it; return an error
    SDFAULT_DO_REBOOT,          // don't write it; reset the board
} sdfault_action_t;

#define SDFAULT_MAX_N 100000000u

typedef struct {
    sdfault_mode_t armed;       // what fires at write `at`; OFF once it has fired
    sdfault_mode_t fired;       // what fired since arming (OFF: nothing yet)
    uint32_t at;                // the write it fires at, counted from arming
    uint32_t writes;            // writes since arming (stops at UINT32_MAX)
    bool counting;              // from arming until sdfault_off()
} sdfault_state_t;

// Arm `mode` (FAIL or REBOOT) at the nth write from now, replacing whatever was armed, and
// start counting from 0. False (nothing changes) if n is 0 or over SDFAULT_MAX_N, or the mode
// isn't one of the two.
bool sdfault_arm(sdfault_mode_t mode, uint32_t n);

// Disarm and stop counting.
void sdfault_off(void);

// One write is about to go to the card: counts it and says what to do with it. A fault fires
// once; later writes pass.
sdfault_action_t sdfault_write(void);

const sdfault_state_t *sdfault_state(void);

// A read fault (`sd fault read <N>`, docs/design/health.md §9.9), counted apart from the writes:
// read request N from now (one per read_blocks_logged() call in sdcard.c) fails with a CRC error
// without reaching the card. A read changes nothing on the card. Arming replaces an armed read
// fault; sdfault_off() disarms both. False (nothing changes) if n is 0 or over SDFAULT_MAX_N.
bool sdfault_arm_read(uint32_t n);

// One read request is about to go to the card: true to fail it instead. Fires once.
bool sdfault_read(void);

// The read fault armed: the read it fires at, and reads since arming; 0 at if none is armed.
void sdfault_read_state(uint32_t *at, uint32_t *reads);

#endif
