// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See rootburn_board.h. THIS FILE PROGRAMS ONE-TIME-PROGRAMMABLE MEMORY. It is compiled only
// into the burn image (-DSOS_BURN_IMAGE=ON), never into everyday firmware; the build fails
// if it is compiled without that option.

#ifndef SOS_BURN_IMAGE
#error "rootburn_board.c programs OTP and belongs only to the burn image (-DSOS_BURN_IMAGE=ON)"
#endif

#include "core/rootburn_board.h"

#include "hardware/regs/otp_data.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/random.h>

#include "core/otpread.h"
#include "core/wdog.h"

// The rows this file is allowed to program: the root secret's list pages, and the lock words
// of those pages. Every write goes through program(), which refuses anything else. This is a
// second fence behind core/rootburn.c's own logic: a bug there can't reach the boot keys,
// the boot flags or any other page.
#define LIST_FIRST_ROW ROOTOTP_ROW(0, 0)
#define LIST_LAST_ROW  ROOTOTP_ROW(ROOTOTP_PAGES - 1, ROOTOTP_PAGE_ROWS - 1)
#define LOCK1_ROW(page) (OTP_DATA_PAGE0_LOCK1_ROW + 2u * (page))

static bool row_allowed(unsigned row) {
    if (row >= LIST_FIRST_ROW && row <= LIST_LAST_ROW)
        return true;
    for (unsigned i = 0; i < ROOTOTP_PAGES; i++)
        if (row == LOCK1_ROW(ROOTOTP_FIRST_PAGE + i))
            return true;
    return false;
}

// The one place OTP is programmed.
static bool program(unsigned row, bool ecc, uint32_t value) {
    if (!row_allowed(row))
        return false;
    otp_cmd_t cmd = { .flags = row | OTP_CMD_WRITE_BITS | (ecc ? OTP_CMD_ECC_BITS : 0) };
    if (ecc) {
        uint16_t v = (uint16_t)value;
        return rom_func_otp_access((uint8_t *)&v, sizeof(v), cmd) == BOOTROM_OK;
    }
    uint32_t v = value & 0xffffffu;
    return rom_func_otp_access((uint8_t *)&v, sizeof(v), cmd) == BOOTROM_OK;
}

// --- reading, as core/rootsource.c does ------------------------------------------------------------

static uint32_t rd_raw(void *ctx, unsigned row) { (void)ctx; return otpread_raw(row); }
static uint16_t rd_ecc(void *ctx, unsigned row) { (void)ctx; return otpread_ecc(row); }
// Read-backs here use the unguarded alias: a row that didn't take must be reported as a
// failed write, not stop the board with a fault in the middle of a burn. The firmware that
// later USES the secret reads it guarded.
static bool rd_careful(void *ctx, unsigned row, unsigned count, uint16_t *out) {
    (void)ctx;
    for (unsigned i = 0; i < count; i++) {
        if (otpread_raw(row + i) == 0xffffffffu)
            return false;
        out[i] = otpread_ecc(row + i);
    }
    return true;
}
static uint32_t rd_lock1(void *ctx, unsigned page) { (void)ctx; return otpread_raw(LOCK1_ROW(page)); }

// --- writing ---------------------------------------------------------------------------------------

static bool wr_ecc(void *ctx, unsigned row, uint16_t value) { (void)ctx; return program(row, true, value); }
static bool wr_raw(void *ctx, unsigned row, uint32_t bits) { (void)ctx; return program(row, false, bits); }
static bool wr_lock1(void *ctx, unsigned page, uint32_t bits) {
    (void)ctx;
    return program(LOCK1_ROW(page), false, bits);
}

static bool get_random(void *ctx, uint8_t *out, size_t len) {
    (void)ctx;
    WC_RNG rng;
    if (wc_InitRng(&rng) != 0)
        return false;
    int rc = wc_RNG_GenerateBlock(&rng, out, (word32)len);
    wc_FreeRng(&rng);
    return rc == 0;
}

// The BOOTSEL button shares a pin with the flash's chip select: float the pin and see whether
// the button pulls it low. Flash can't be used meanwhile, so this runs from RAM with
// interrupts off. The technique is the Pico SDK examples' (picoboard/button, BSD-3-Clause,
// Raspberry Pi (Trading) Ltd).
bool __no_inline_not_in_flash_func(rootburn_button)(void) {
    const unsigned cs_pin = 1;
    uint32_t flags = save_and_disable_interrupts();
    hw_write_masked(&ioqspi_hw->io[cs_pin].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    for (volatile int i = 0; i < 1000; ++i)
        ;
    bool pressed = !(sio_hw->gpio_hi_in & SIO_GPIO_HI_IN_QSPI_CSN_BITS);
    hw_write_masked(&ioqspi_hw->io[cs_pin].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    restore_interrupts(flags);
    return pressed;
}

static bool present(void *ctx) { (void)ctx; return rootburn_button(); }

// A burn is some forty boot ROM writes in one call, and how long each takes on this chip
// isn't known yet. The watchdog must not reset the board in the middle.
static void tick(void *ctx) { (void)ctx; wdog_feed(); }

static const rootburn_ops_t ops = {
    NULL, { NULL, rd_raw, rd_ecc, rd_careful, rd_lock1 },
    wr_ecc, wr_raw, wr_lock1, tick, get_random, present,
};

void rootburn_plan(rootburn_report_t *rep, unsigned *pages_to_lock) {
    unsigned index = 0;
    rep->result = ROOTBURN_NO_PAGE;
    rep->page = rep->row = 0;
    rep->wrote = rep->page_locked = false;
    *pages_to_lock = 0;
    rep->nothing_landed = false;
    bool found = false;
    switch (roototp_find(&ops.reader, &index)) {
    case ROOTOTP_NONE_YET:
        rep->result = ROOTBURN_OK;
        break;
    case ROOTOTP_FOUND:
        rep->result = ROOTBURN_HAS_SECRET;
        found = true;
        break;
    default:
        return;
    }
    rep->page = ROOTOTP_FIRST_PAGE + index;
    // The pages a run would lock: written ones the bootloader can still write, before the page
    // it would use, or up to and including the secret's if there is one. Never a blank one.
    for (unsigned i = 0; i < ROOTOTP_PAGES && (found ? i <= index : i < index); i++) {
        bool blank = true;
        for (unsigned row = 0; row < ROOTOTP_PAGE_ROWS && blank; row++)
            blank = otpread_raw(ROOTOTP_ROW(i, row)) == 0;
        if (!blank && roototp_bootloader_can_write(rd_lock1(NULL, ROOTOTP_FIRST_PAGE + i)))
            (*pages_to_lock)++;
    }
    if (found && *pages_to_lock)
        rep->result = ROOTBURN_RELOCKED;
}

void rootburn_board_run(bool rehearse, rootburn_report_t *rep) {
    rootburn_run(&ops, rehearse, rep);
}
