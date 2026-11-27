// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The firmware's health checks (core/health.h): the table, and steps that read what SOS already
// keeps. The judging is in health.c (pure, host tested); these only gather the numbers.
//
// Phase 1's seed checks (docs/design/health.md §9) read state only, take microseconds each, and
// hold no gate. Phase 2's boot checks (§10) test the hardware and the crypto before anything
// makes a key: `clocks`, `flash`, `trng` (holds KEYS) and `crypto` (holds KEYS and CRYPTO). Their
// judging is in hwtest.c and the crypto vectors in selftest.c. There is no full-tier check yet:
// `health full` runs the same checks as `health run`, plus the test-only gate checks of a test
// build.

#include "core/health.h"

#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/resets.h"
#include "hardware/structs/trng.h"
#include "hardware/sync.h"
#include "pico/flash.h"
#include "pico/stdlib.h"

#include "core/fault.h"
#include "core/glitchdet.h"
#include "core/heap.h"
#include "core/hwtest.h"
#include "core/selftest.h"
#include "core/sensors.h"
#include "core/stackmon.h"
#include "core/wdog.h"
#include "lua/lua_core1.h"

#define SEED_PERIOD_S 60                // the background seed checks: once a minute

static health_step_t check_resets(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (!cancel)
        health_judge_resets(fault_last(), glitchdet_caused_reset(), wdog_caused_reset(), out);
    return HEALTH_DONE;
}

static health_step_t check_glitch(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (!cancel)
        health_judge_glitch(glitchdet_guard_tripped(), out);
    return HEALTH_DONE;
}

// stackmon_peak() scans core 0's painted stack for the deepest word changed: about 8 KB of
// reads, well under a millisecond.
static health_step_t check_stack(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (!cancel)
        health_judge_stack(stackmon_peak(), stackmon_size(), lua_core1_stack_peak(),
                           LUA_CORE1_STACK, out);
    return HEALTH_DONE;
}

static health_step_t check_heap(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (cancel)
        return HEALTH_DONE;
    int self = health_find("heap");
    const health_entry_t *e = self >= 0 ? health_entry((unsigned)self) : NULL;
    health_judge_heap(heap_free(), e ? e->result.status : HEALTH_NONE, out);
    return HEALTH_DONE;
}

// The latest reading (sensors_poll takes one every SENSORS_SAMPLE_S): no ADC access here.
static health_step_t check_temp(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (!cancel)
        health_judge_temp(sensors_last_temp(), out);
    return HEALTH_DONE;
}

// The lowest VSYS since the last run. Run again within SENSORS_SAMPLE_S (`health run` just after
// the background run) there is none new: the latest reading stands in.
static health_step_t check_supply(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (cancel)
        return HEALTH_DONE;
    int min = sensors_vsys_min_take();
    if (min == SENSORS_NONE)
        min = sensors_last_vsys();
    health_judge_supply(min, sensors_usb_power(), out);
    return HEALTH_DONE;
}

// --- phase 2: the boot tier's hardware and crypto checks ---------------------------------------

// clocks: each running clock through the frequency counter (FC0, about 1 ms a measurement)
// against what the SDK set, and the LPOSC as the cross-check on the crystal (hwtest.h says what
// this can and can't see). clk_ref isn't on the list: FC0 counts against it, so it would always
// match itself.
static health_step_t check_clocks(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (cancel)
        return HEALTH_DONE;
    static const struct {
        const char *name;
        clock_handle_t clk;
        uint fc0;
    } list[] = {
        {"clk_sys", clk_sys, CLOCKS_FC0_SRC_VALUE_CLK_SYS},
        {"clk_peri", clk_peri, CLOCKS_FC0_SRC_VALUE_CLK_PERI},
        {"clk_usb", clk_usb, CLOCKS_FC0_SRC_VALUE_CLK_USB},
        {"clk_adc", clk_adc, CLOCKS_FC0_SRC_VALUE_CLK_ADC},
        {"clk_hstx", clk_hstx, CLOCKS_FC0_SRC_VALUE_CLK_HSTX},
    };
    hwtest_clock_t c[sizeof(list) / sizeof(list[0])];
    for (unsigned i = 0; i < sizeof(list) / sizeof(list[0]); i++) {
        c[i].name = list[i].name;
        c[i].want_hz = clock_get_hz(list[i].clk);
        c[i].got_khz = c[i].want_hz ? frequency_count_khz(list[i].fc0) : 0;
    }
    uint32_t lposc = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_LPOSC_CLKSRC);
    hwtest_judge_clocks(c, sizeof(c) / sizeof(c[0]), lposc, out);
    return HEALTH_DONE;
}

// flash: the JEDEC ID (command 0x9f). XIP is off while it runs, so it needs interrupts off and
// core 1 stopped, as flash_get_unique_id() does: flash_safe_execute(), which flashop_run() wraps
// too (not used here, so `uptime`'s count of flash writes stays a count of writes).
// flash_do_cmd() is in RAM and turns XIP back on before it returns; tens of microseconds.
#define FLASH_LOCKOUT_MS 1000           // as flashop.c: how long to wait for core 1 to stop

static void read_jedec(void *arg) {
    uint8_t tx[4] = {0x9f, 0, 0, 0}, rx[4] = {0};
    flash_do_cmd(tx, rx, sizeof(tx));
    memcpy(arg, rx + 1, 3);
}

static health_step_t check_flash(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (cancel)
        return HEALTH_DONE;
    uint8_t id[3] = {0};
    if (flash_safe_execute(read_jedec, id, FLASH_LOCKOUT_MS) != PICO_OK) {
        out->status = HEALTH_SKIP;
        snprintf(out->text, sizeof(out->text), "core 1 didn't stop for the flash command");
        return HEALTH_DONE;
    }
    hwtest_judge_flash(id, PICO_FLASH_SIZE_BYTES, out);
    return HEALTH_DONE;
}

// trng: SP 800-90B's startup tests over raw samples of the TRNG's ring oscillator (hwtest.h).
// wolfCrypt is seeded from pico_rand, which mixes the TRNG with other sources: testing its
// output would test the mixing, not the noise source. So this reads the TRNG block itself:
//   - one sample every TRNG_SAMPLE_CLOCKS clk_sys cycles (the datasheet's minimum with the von
//     Neumann corrector bypassed is 17; larger is better), the shortest inverter chain;
//   - the corrector bypassed, so the bits are the oscillator's own; the TRNG's own CRNGT and
//     autocorrelation tests left on, and their errors counted (hwtest_judge_trng);
//   - 16 blocks of 192 bits: about 1.3 ms at 150 MHz.
// pico_rand reads the same block (on either core, under PICO_SPINLOCK_ID_RAND, with all checks
// bypassed and one sample per cycle): this takes the same lock, which also keeps interrupts off,
// resets the block before and after, and so leaves it as pico_rand finds it after a reset. Its
// next refill sets every register it relies on.
#define TRNG_SAMPLE_CLOCKS 64
#define TRNG_TIMEOUT_US    10000        // 16 blocks take about 1.3 ms

static void trng_collect(hwtest_trng_t *t, uint32_t *words) {
    spin_lock_t *lock = spin_lock_instance(PICO_SPINLOCK_ID_RAND);
    uint32_t save = spin_lock_blocking(lock);
    reset_unreset_block_num_wait_blocking(RESET_TRNG);
    trng_hw->trng_config = 0;
    trng_hw->sample_cnt1 = TRNG_SAMPLE_CLOCKS;
    trng_hw->trng_debug_control = TRNG_TRNG_DEBUG_CONTROL_VNC_BYPASS_BITS;
    trng_hw->rng_icr = TRNG_RNG_ICR_BITS;
    trng_hw->rnd_source_enable = TRNG_RND_SOURCE_ENABLE_RND_SRC_EN_BITS;
    uint64_t t0 = time_us_64();
    while (t->blocks < HWTEST_TRNG_BLOCKS && time_us_64() - t0 < TRNG_TIMEOUT_US) {
        uint32_t isr = trng_hw->rng_isr;
        if (isr & TRNG_RNG_ISR_AUTOCORR_ERR_BITS) {
            t->autocorr_error = true;   // stopped until a reset
            break;
        }
        if (isr & (TRNG_RNG_ISR_CRNGT_ERR_BITS | TRNG_RNG_ISR_VN_ERR_BITS)) {
            // The block is thrown away: start the next one.
            if (++t->crngt_errors > HWTEST_TRNG_CRNGT_MAX)
                break;
            trng_hw->rng_icr = TRNG_RNG_ICR_CRNGT_ERR_BITS | TRNG_RNG_ICR_VN_ERR_BITS;
            trng_hw->rnd_source_enable = 0;
            trng_hw->rnd_source_enable = TRNG_RND_SOURCE_ENABLE_RND_SRC_EN_BITS;
            continue;
        }
        if (isr & TRNG_RNG_ISR_EHR_VALID_BITS) {
            // Reading the last word clears the block and starts the next.
            uint32_t *w = words + t->blocks * HWTEST_TRNG_BLOCK_WORDS;
            for (unsigned i = 0; i < HWTEST_TRNG_BLOCK_WORDS; i++)
                w[i] = trng_hw->ehr_data[i];
            trng_hw->rng_icr = TRNG_RNG_ICR_EHR_VALID_BITS;
            t->blocks++;
        }
    }
    t->us = (uint32_t)(time_us_64() - t0);
    trng_hw->rnd_source_enable = 0;
    reset_unreset_block_num_wait_blocking(RESET_TRNG);
    spin_unlock(lock, save);
}

static health_step_t check_trng(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (cancel)
        return HEALTH_DONE;
    uint32_t words[HWTEST_TRNG_WORDS];
    hwtest_trng_t t;
    memset(&t, 0, sizeof(t));
    trng_collect(&t, words);
    hwtest_trng_stats(words, t.blocks * HWTEST_TRNG_BLOCK_WORDS, &t.stats);
    hwtest_judge_trng(&t, out);
    return HEALTH_DONE;
}

// crypto: known answers for the algorithms SOS relies on (selftest.c), about 1.7 KB of heap
// while it runs. The time is in the text: the boot tier's budget is 100 ms, and Ed25519 and
// X25519 take most of it.
static health_step_t check_crypto(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (cancel)
        return HEALTH_DONE;
    uint64_t t0 = time_us_64();
    unsigned ran;
    const char *why = selftest_crypto(&ran);
    unsigned ms = (unsigned)((time_us_64() - t0 + 500) / 1000);
    if (why) {
        out->status = HEALTH_FAIL;
        snprintf(out->text, sizeof(out->text), "%s", why);
    } else {
        out->status = HEALTH_PASS;
        snprintf(out->text, sizeof(out->text), "%u known answers (sha2 hmac gcm xts 25519) in %u ms",
                 ran, ms);
    }
    return HEALTH_DONE;
}

static const health_check_t checks[] = {
    {"resets", HEALTH_BOOT, 0, 0, 0, check_resets},
    {"clocks", HEALTH_BOOT, 0, 0, 0, check_clocks},
    {"flash", HEALTH_BOOT, 0, 0, 0, check_flash},
    {"trng", HEALTH_BOOT, 0, 0, HEALTH_GATE_KEYS, check_trng},
    {"crypto", HEALTH_BOOT, 0, 0, HEALTH_GATE_KEYS | HEALTH_GATE_CRYPTO, check_crypto},
    {"glitch", HEALTH_BACKGROUND, SEED_PERIOD_S, 0, 0, check_glitch},
    {"stack", HEALTH_BACKGROUND, SEED_PERIOD_S, 0, 0, check_stack},
    {"heap", HEALTH_BACKGROUND, SEED_PERIOD_S, 0, 0, check_heap},
    {"chip-temp", HEALTH_BACKGROUND, SEED_PERIOD_S, 0, 0, check_temp},
    {"supply", HEALTH_BACKGROUND, SEED_PERIOD_S, 0, 0, check_supply},
};

void health_boot(void) {
    health_init(checks, sizeof(checks) / sizeof(checks[0]), time_us_64);
    health_run_boot();
}

#ifdef SOS_TEST_COMMANDS
// The checks `health inject gate` adds: each holds one gate, and is set to FAIL by hand. Run by
// `health full` (they are full-tier), they PASS, which opens the gate again.
static health_step_t check_injected(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (!cancel) {
        out->status = HEALTH_PASS;
        snprintf(out->text, sizeof(out->text), "test-only check (health inject gate)");
    }
    return HEALTH_DONE;
}

static const health_check_t inject_keys = {
    "inj-keys", HEALTH_FULL, 0, 0, HEALTH_GATE_KEYS, check_injected,
};
static const health_check_t inject_crypto = {
    "inj-crypto", HEALTH_FULL, 0, 0, HEALTH_GATE_CRYPTO, check_injected,
};

bool health_board_inject_gate(const char *gate) {
    const health_check_t *c = !strcmp(gate, "keys") ? &inject_keys
                            : !strcmp(gate, "crypto") ? &inject_crypto : NULL;
    if (!c || (health_find(c->name) < 0 && !health_add(c)))
        return false;
    return health_inject(c->name, HEALTH_FAIL, "set by hand (health inject gate)");
}
#endif
