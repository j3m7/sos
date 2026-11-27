// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "core/fault.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/structs/m33.h"
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "pico/platform.h"

// The record: watchdog scratch registers 0-3 (the SDK uses 4-7 for reboots).
//   [0] magic (high 16 bits), kind, core
//   [1] program counter (fault) or the caller of panic()
//   [2] link register (fault) or the panic format string's address
//   [3] CFSR (fault) or the build the string belongs to (panic)
#define MAGIC        0x50f70000u
#define KIND_FAULT   1u
#define KIND_PANIC   2u
#ifdef SOS_TEST_COMMANDS
#define KIND_TEST    3u     // fault_test_reset(): [1] the card write it fired before
#endif

#define CFSR_STKOF       (1u << 20)     // UsageFault: stack limit (MSPLIM) reached
#define CFSR_UNDEFINSTR  (1u << 16)
#define CFSR_DIVBYZERO   (1u << 25)
#define CFSR_BUS         (0xffu << 8)   // BusFault status
#define CFSR_MEM         0xffu          // MemManage status

static char last[128];
static bool have_last;

static void __attribute__((noreturn)) record_and_reset(uint32_t kind, uint32_t a, uint32_t b,
                                                       uint32_t c) {
    watchdog_hw->scratch[0] = MAGIC | kind << 8 | get_core_num();
    watchdog_hw->scratch[1] = a;
    watchdog_hw->scratch[2] = b;
    watchdog_hw->scratch[3] = c;
    watchdog_reboot(0, 0, 0);           // resets the whole chip at once
    for (;;)
        ;
}

// --- hard fault ----------------------------------------------------------------------------------

// The handler's own stack: after a stack overflow the faulting stack has no room left.
static uint32_t fault_stack[128];

static void __attribute__((noreturn, used)) hardfault_c(const uint32_t *frame) {
    uint32_t cfsr = m33_hw->cfsr;
    uint32_t pc = 0, lr = 0;
    // The exception frame (r0-r3, r12, lr, pc, xpsr), if it's where a stack can be. After a
    // stack overflow it wasn't pushed, so what's there isn't this fault's.
    uintptr_t f = (uintptr_t)frame;
    if (!(cfsr & CFSR_STKOF) && !(f & 3) && f >= SRAM_BASE && f + 32 <= SRAM_END) {
        lr = frame[5];
        pc = frame[6];
    }
    record_and_reset(KIND_FAULT, pc, lr, cfsr);
}

// All code here runs on the main stack.
static void __attribute__((naked)) hardfault_entry(void) {
    __asm volatile(
        "mov r0, sp\n"                  // the exception frame
        "movs r1, #0\n"
        "msr msplim, r1\n"              // no stack limit for the handler
        "ldr r1, =fault_stack_end\n"
        "ldr r1, [r1]\n"
        "mov sp, r1\n"
        "b hardfault_c\n"
        ".ltorg\n");
}

static uint32_t *const fault_stack_end __attribute__((used)) =
    fault_stack + sizeof(fault_stack) / sizeof(fault_stack[0]);

// --- panic (PICO_PANIC_FUNCTION, see CMakeLists.txt) -----------------------------------------------

void __attribute__((noreturn)) sos_panic(const char *fmt, ...) {
    record_and_reset(KIND_PANIC, (uint32_t)(uintptr_t)__builtin_return_address(0),
                     (uint32_t)(uintptr_t)fmt, (uint32_t)SOS_BUILD_EPOCH);
}

#ifdef SOS_TEST_COMMANDS
void fault_test_reset(uint32_t write_no) {
    record_and_reset(KIND_TEST, write_no, 0, 0);
}
#endif

// --- next boot -----------------------------------------------------------------------------------

void fault_boot(void) {
    extern char __flash_binary_start, __flash_binary_end;
    // Take over the hard fault vector at run time: the vector table is in RAM and shared by
    // both cores. (It began this way because the SD card library's crash.c defined
    // isr_hardfault itself, so the handler couldn't be replaced at link time. SOS no longer
    // builds that file, but this still works for whatever is linked in.)
    ((uint32_t *)m33_hw->vtor)[3] = (uint32_t)(uintptr_t)hardfault_entry;

    uint32_t head = watchdog_hw->scratch[0];
    uint32_t a = watchdog_hw->scratch[1], b = watchdog_hw->scratch[2], c = watchdog_hw->scratch[3];
    have_last = false;
    if ((head & 0xffff0000u) != MAGIC)
        return;
    watchdog_hw->scratch[0] = 0;        // reported once
    unsigned kind = (head >> 8) & 0xff, core = head & 0xff;

    if (kind == KIND_FAULT) {
        const char *what = c & CFSR_STKOF ? "stack overflow"
                         : c & CFSR_UNDEFINSTR ? "undefined instruction"
                         : c & CFSR_DIVBYZERO ? "division by zero"
                         : c & (CFSR_BUS | CFSR_MEM) ? "bad memory access" : "fault";
        if (a)
            snprintf(last, sizeof(last), "hard fault on core %u at 0x%08lx, called from 0x%08lx (%s, CFSR 0x%08lx)",
                     core, (unsigned long)a, (unsigned long)b, what, (unsigned long)c);
        else
            snprintf(last, sizeof(last), "hard fault on core %u (%s, CFSR 0x%08lx)", core, what,
                     (unsigned long)c);
    } else if (kind == KIND_PANIC) {
        // The message is a string in the firmware image: only readable if this is the same build.
        const char *msg = (const char *)(uintptr_t)b;
        bool same = c == (uint32_t)SOS_BUILD_EPOCH && msg >= &__flash_binary_start &&
                    msg < &__flash_binary_end;
        snprintf(last, sizeof(last), "panic on core %u, called from 0x%08lx: %.60s", core,
                 (unsigned long)a, same ? msg : "(message from another build)");
        for (char *p = last; *p; p++)
            if (*p == '\n')
                *p = ' ';
#ifdef SOS_TEST_COMMANDS
    } else if (kind == KIND_TEST) {
        snprintf(last, sizeof(last), "test reset: 'sd fault reboot' fired before card write %lu",
                 (unsigned long)a);
#endif
    } else {
        return;
    }
    have_last = true;
}

const char *fault_last(void) {
    return have_last ? last : NULL;
}

// --- testing -------------------------------------------------------------------------------------

static int recurse(int n);
static int (*volatile recurse_p)(int) = recurse;    // through a pointer: not optimized away

static int __attribute__((noinline)) recurse(int n) {
    volatile char pad[64];
    pad[0] = (char)n;
    return recurse_p(n + 1) + pad[0];
}

void fault_test(const char *how) {
    if (!strcmp(how, "fault"))
        __asm volatile("udf #0");
    else if (!strcmp(how, "stack"))
        recurse_p(0);
    else if (!strcmp(how, "panic"))
        panic("test panic from the 'crash' command");
    else if (!strcmp(how, "hang"))
        for (;;)
            ;
}
