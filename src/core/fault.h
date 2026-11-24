// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// What stopped the firmware, kept across the reset it causes. A hard fault (bad memory access,
// undefined instruction, stack overflow, on either core) or an SDK panic stores a short record
// in the watchdog's scratch registers, which survive a watchdog reset, and resets the chip at
// once. The next boot reads the record, logs it and clears it.
//
// Without this both ended in a breakpoint loop until the watchdog timeout (or for good, for a
// fault on core 1, which core 0 doesn't notice), and the next boot only knew "watchdog timeout".

#ifndef SOS_FAULT_H
#define SOS_FAULT_H

// Read and clear the record left by the previous run. Call once, early in main().
void fault_boot(void);

// What the record said, e.g. "hard fault on core 1 at 0x1000a3c4 (stack overflow)"; NULL if the
// last reset wasn't one of these.
const char *fault_last(void);

// For testing: stop in the named way ("fault", "stack", "panic", "hang"). Returns only if `how`
// isn't one of them.
void fault_test(const char *how);

#endif
