// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The burn image's hardware side: the only code in SOS that can program OTP memory.
// Compiled only with -DSOS_BURN_IMAGE=ON. See core/rootburn.h and docs/securing-a-board.md.

#ifndef SOS_ROOTBURN_BOARD_H
#define SOS_ROOTBURN_BOARD_H

#include <stdbool.h>

#include "core/rootburn.h"

// Is the BOOTSEL button held right now? Safe to call from the main loop; it stops interrupts
// and flash access for a few microseconds.
bool rootburn_button(void);

// What a run would do, without writing anything: the result it would start from
// (ROOTBURN_OK meaning "a blank page is ready"), the page it would use, and how many earlier
// pages it would lock first.
void rootburn_plan(rootburn_report_t *report, unsigned *pages_to_lock);

// Run it for real on this board's OTP. The button must be held when it starts.
void rootburn_board_run(bool rehearse, rootburn_report_t *report);

#endif
