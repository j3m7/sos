// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The burn image's hardware side: the only code in SOS that can program OTP memory.
// Compiled only with -DSOS_BURN_IMAGE=ON. See core/rootburn.h and docs/security/design.md.

#ifndef SOS_ROOTBURN_BOARD_H
#define SOS_ROOTBURN_BOARD_H

#include <stdbool.h>

#include "core/rootburn.h"

// Is the BOOTSEL button held right now? It stops interrupts and floats the flash's chip select
// for a few microseconds, so nothing may be executing from flash meanwhile: fine from the main
// loop on one core, as SOS runs; not with code running from flash on the other core.
bool rootburn_button(void);

// What a run would do, without writing anything: ROOTBURN_OK (a blank page is ready: `page`),
// ROOTBURN_HAS_SECRET (the secret is there and locked: nothing to do), ROOTBURN_RELOCKED (the
// secret is there and a run would only lock pages), or ROOTBURN_NO_PAGE; and how many written
// pages it would lock.
void rootburn_plan(rootburn_report_t *report, unsigned *pages_to_lock);

// Run it for real on this board's OTP. The button must be held when it starts.
void rootburn_board_run(bool rehearse, rootburn_report_t *report);

#endif
