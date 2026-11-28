// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The firmware's health checks (core/health.h): the table, and steps that read what SOS already
// keeps. The judging is in health.c (pure, host tested); these only gather the numbers.
//
// Phase 1's seed checks (docs/design/health.md §9) read state only, take microseconds each, and
// hold no gate. Phase 2's boot checks (§10) test the hardware and the crypto before anything
// makes a key: `clocks`, `flash`, `trng` (holds KEYS) and `crypto` (holds KEYS and CRYPTO), with
// `crypto-ssh` (background) for the known answers too slow for boot. Their judging is in
// hwtest.c and the crypto vectors in selftest.c. Phase 3's checks of stored state (§9.3:
// `config`, `rootsecret`, `otp`, `failcount`, `hostkey`, `card`) are background checks in
// statecheck_board.c, judged in statecheck.c; none holds a gate. Phase 4's (§9.5: `resets` with
// every reset reason and the reset history, `core1`, `heap-sum`, `rtc-drift`, and the brownout
// detector's setting in `supply`) are read in bgcheck_board.c and judged in bgcheck.c; none holds
// a gate either. Phase 5's full tier (§9.7: `ram`, `image`, `sd-check`) runs only on `health
// full` or `health run <name>`; its steps are in fullcheck_board.c, judged in fullcheck.c. `ram`
// and `image` hold KEYS and CRYPTO, closed by their FAILs only; `sd-check` holds none. Phase 6's
// counters (§9.9: `wifi`, `sd-bus`, `net-pools`) judge what the Wi-Fi driver's hooks, wifi.c,
// sdcard.c and lwIP counted, in countcheck_board.c, judged in countcheck.c; none holds a gate.

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

#include "core/bgcheck.h"
#include "core/bgcheck_board.h"
#include "core/countcheck_board.h"
#include "core/fullcheck_board.h"
#include "core/glitchdet.h"
#include "core/heap.h"
#include "core/heaptag.h"
#include "core/hwtest.h"
#include "core/log.h"
#include "core/selftest.h"
#include "core/sensors.h"
#include "core/stackmon.h"
#include "core/statecheck.h"
#include "files/card_check.h"
#include "files/fsck_check.h"
#if SOS_HAVE_LUA
#include "lua/lua_check.h"
#endif
#if SOS_HAVE_NET
#include "net/net_check.h"
#endif
#if SOS_HAVE_SSH
#include "ssh/hostkey_check.h"
#endif
#if SOS_HAVE_LUA
#include "lua/lua_core1.h"
#endif

#define SEED_PERIOD_S 60                // the background seed checks: once a minute

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
#if SOS_HAVE_LUA
        health_judge_stack(stackmon_peak(), stackmon_size(), lua_core1_stack_peak(),
                           LUA_CORE1_STACK, out);
#else
        health_judge_stack(stackmon_peak(), stackmon_size(), 0, 0, out);   // core 0 alone
#endif
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
    bgcheck_judge_bod(bgcheck_bod_register(), out);     // phase 4: the brownout detector's setting
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
// output would test the mixing, not the noise source. So this reads the TRNG block itself, as
// docs/design/health.md §9.2 explains:
//   - one sample every 256 clk_sys cycles (the datasheet: larger periods give better bits, and
//     17 is the least with the von Neumann corrector bypassed), the shortest inverter chain;
//   - the corrector bypassed, so the bits are the oscillator's own, and the block's own
//     autocorrelation test bypassed too: on raw bits it stopped the TRNG on dev-1 at 64 cycles
//     (#242), so SP 800-90B's tests over the raw bits are the check, with the block's CRNGT;
//   - 16 blocks of 192 bits: about 5.3 ms at 150 MHz; interrupts are off for at most
//     TRNG_TIMEOUT_US, the bound if the TRNG stalls.
// pico_rand reads the same block (on either core, under PICO_SPINLOCK_ID_RAND, with all checks
// bypassed and one sample per cycle): this takes the same lock, which also keeps interrupts off,
// resets the block before and after, and so leaves it as pico_rand finds it after a reset. Its
// next refill sets every register it relies on.
typedef struct {
    uint32_t sample;                    // clk_sys cycles between samples (SAMPLE_CNT1)
    bool autocorr;                      // the block's autocorrelation test on
    bool vn;                            // the von Neumann corrector on
} trng_cfg_t;

static const trng_cfg_t trng_check_cfg = {256, false, false};
#define TRNG_TIMEOUT_US 20000           // 16 blocks take about 5.3 ms

// Collect up to HWTEST_TRNG_BLOCKS blocks into words, stopping at the timeout, an
// autocorrelation error, or more than err_max CRNGT and von Neumann errors together.
static void trng_collect(const trng_cfg_t *cfg, uint32_t timeout_us, unsigned err_max,
                         hwtest_trng_t *t, uint32_t *words) {
    spin_lock_t *lock = spin_lock_instance(PICO_SPINLOCK_ID_RAND);
    uint32_t save = spin_lock_blocking(lock);
    reset_unreset_block_num_wait_blocking(RESET_TRNG);
    trng_hw->trng_config = 0;
    trng_hw->sample_cnt1 = cfg->sample;
    uint32_t bypass = 0;
    if (!cfg->vn)
        bypass |= TRNG_TRNG_DEBUG_CONTROL_VNC_BYPASS_BITS;
    if (!cfg->autocorr)
        bypass |= TRNG_TRNG_DEBUG_CONTROL_AUTO_CORRELATE_BYPASS_BITS;
    trng_hw->trng_debug_control = bypass;
    trng_hw->rng_icr = TRNG_RNG_ICR_BITS;
    trng_hw->rnd_source_enable = TRNG_RND_SOURCE_ENABLE_RND_SRC_EN_BITS;
    uint64_t t0 = time_us_64();
    while (t->blocks < HWTEST_TRNG_BLOCKS && time_us_64() - t0 < timeout_us) {
        uint32_t isr = trng_hw->rng_isr;
        if (isr & TRNG_RNG_ISR_AUTOCORR_ERR_BITS) {
            t->autocorr_error = true;   // stopped until a reset
            break;
        }
        if (isr & (TRNG_RNG_ISR_CRNGT_ERR_BITS | TRNG_RNG_ISR_VN_ERR_BITS)) {
            // The block is thrown away: start the next one.
            if (isr & TRNG_RNG_ISR_CRNGT_ERR_BITS)
                t->crngt_errors++;
            if (isr & TRNG_RNG_ISR_VN_ERR_BITS)
                t->vn_errors++;
            if (t->crngt_errors + t->vn_errors > err_max)
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

static void trng_run(const trng_cfg_t *cfg, uint32_t timeout_us, unsigned err_max,
                     hwtest_trng_t *t) {
    uint32_t words[HWTEST_TRNG_WORDS];
    memset(t, 0, sizeof(*t));
    trng_collect(cfg, timeout_us, err_max, t, words);
    hwtest_trng_stats(words, t->blocks * HWTEST_TRNG_BLOCK_WORDS, &t->stats);
}

static health_step_t check_trng(void **state, bool cancel, health_result_t *out) {
    (void)state;
    if (cancel)
        return HEALTH_DONE;
    hwtest_trng_t t;
    trng_run(&trng_check_cfg, TRNG_TIMEOUT_US, HWTEST_TRNG_CRNGT_MAX, &t);
    hwtest_judge_trng(&t, out);
    return HEALTH_DONE;
}

// crypto and crypto-ssh: selftest.c's known answers, about 2 KB of heap while they run.
// `crypto` (boot tier) is the boot subset: what making keys and sealing at boot rely on.
// `crypto-ssh` (background, first 30 s after boot, then daily) is the rest: Ed25519 signing and
// verifying and X25519, tens of milliseconds each, too slow for the boot tier's 100 ms. Both
// hold KEYS and CRYPTO. At boot `crypto` runs in one call (a boot check must); anywhere else
// each runs one test per step, so no step holds the main loop for more than one test. The
// text gives the time per group; a passing run is also logged at info, so dmesg keeps the
// figures.
typedef struct {
    selftest_t *st;
    unsigned next;                      // the next test to look at
    unsigned ran;
    uint32_t total_us;
    uint32_t group_us[SELFTEST_GROUPS];
    unsigned group_runs[SELFTEST_GROUPS];
} kat_state_t;

static bool booting;                    // health_boot() is running the boot tier

static void kat_free(void **state) {
    kat_state_t *k = *state;
    if (k) {
        selftest_close(k->st);
        heap_release(HEAP_NET, k);
    }
    *state = NULL;
}

// The next test of the subset from index i on, or selftest_count().
static unsigned kat_next(bool boot_subset, unsigned i) {
    while (i < selftest_count() && selftest_info(i)->boot != boot_subset)
        i++;
    return i;
}

static health_step_t kat_step(const char *name, bool boot_subset, void **state, bool cancel,
                              health_result_t *out) {
    if (cancel) {
        kat_free(state);
        return HEALTH_DONE;
    }
    kat_state_t *k = *state;
    if (!k) {
        k = heap_zalloc(HEAP_NET, sizeof(*k));
        if (k)
            k->st = selftest_open();
        *state = k;
        if (!k || !k->st) {
            kat_free(state);
            out->status = HEALTH_FAIL;
            snprintf(out->text, sizeof(out->text), "no memory to run the tests");
            return HEALTH_DONE;
        }
    }
    do {
        unsigned i = kat_next(boot_subset, k->next);
        if (i >= selftest_count())
            break;
        k->next = i + 1;
        uint64_t t0 = time_us_64();
        const char *why = selftest_run(k->st, i);
        uint32_t us = (uint32_t)(time_us_64() - t0);
        selftest_group_t g = selftest_info(i)->group;
        k->ran++;
        k->total_us += us;
        k->group_us[g] += us;
        k->group_runs[g]++;
        if (why) {
            out->status = HEALTH_FAIL;
            snprintf(out->text, sizeof(out->text), "%s", why);
            kat_free(state);
            return HEALTH_DONE;
        }
    } while (booting);
    if (kat_next(boot_subset, k->next) < selftest_count())
        return HEALTH_MORE;
    out->status = HEALTH_PASS;
    selftest_text(out->text, sizeof(out->text), k->ran, k->total_us, k->group_us, k->group_runs);
    log_msg(SOS_LOG_KERN | SOS_LOG_INFO, "health: %s: %s", name, out->text);
    kat_free(state);
    return HEALTH_DONE;
}

static health_step_t check_crypto(void **state, bool cancel, health_result_t *out) {
    return kat_step("crypto", true, state, cancel, out);
}

#if SOS_HAVE_SSH || SOS_HAVE_TLS
static health_step_t check_crypto_ssh(void **state, bool cancel, health_result_t *out) {
    return kat_step("crypto-ssh", false, state, cancel, out);
}
#endif

#define CRYPTO_SSH_FIRST_S  30          // after boot: Wi-Fi is coming up, nobody logged in yet
#define CRYPTO_SSH_PERIOD_S 86400       // then daily
// Phase 3 (docs/design/health.md §9.3): after the configuration, the host key and the card are
// up, staggered so the first minute isn't one burst; then hourly, the card every 6 hours.
#define STATE_PERIOD_S      3600
#define CARD_PERIOD_S       (6 * 3600)
// Phase 4 (§9.5): the heartbeat every 10 s (its WARN is three runs without a beat), the heap's
// totals every 5 minutes, the drift record (it changes only at a sync) every 10 minutes.
#define CORE1_PERIOD_S      10
#define HEAPSUM_PERIOD_S    300
#define DRIFT_PERIOD_S      600
// Phase 6 (§9.9): the counters every 10 minutes, after phase 4's first runs and Wi-Fi's connect.
#define COUNT_PERIOD_S      600

static const health_check_t checks[] = {
    {"resets", HEALTH_BOOT, 0, 0, 0, bgcheck_resets_step},
    {"clocks", HEALTH_BOOT, 0, 0, 0, check_clocks},
    {"flash", HEALTH_BOOT, 0, 0, 0, check_flash},
    {"trng", HEALTH_BOOT, 0, 0, HEALTH_GATE_KEYS, check_trng},
    {"crypto", HEALTH_BOOT, 0, 0, HEALTH_GATE_KEYS | HEALTH_GATE_CRYPTO, check_crypto},
#if SOS_HAVE_SSH || SOS_HAVE_TLS
    // Ed25519 is SSH's, X25519 SSH's and TLS's (docs/design/build-features.md §4.3).
    {"crypto-ssh", HEALTH_BACKGROUND, CRYPTO_SSH_PERIOD_S, CRYPTO_SSH_FIRST_S,
     HEALTH_GATE_KEYS | HEALTH_GATE_CRYPTO, check_crypto_ssh},
#endif
    {"glitch", HEALTH_BACKGROUND, SEED_PERIOD_S, 0, 0, check_glitch},
    {"stack", HEALTH_BACKGROUND, SEED_PERIOD_S, 0, 0, check_stack},
    {"heap", HEALTH_BACKGROUND, SEED_PERIOD_S, 0, 0, check_heap},
    {"chip-temp", HEALTH_BACKGROUND, SEED_PERIOD_S, 0, 0, check_temp},
    {"supply", HEALTH_BACKGROUND, SEED_PERIOD_S, 0, 0, check_supply},
    {"config", HEALTH_BACKGROUND, STATE_PERIOD_S, 40, 0, statecheck_config_step},
    {"rootsecret", HEALTH_BACKGROUND, STATE_PERIOD_S, 45, 0, statecheck_root_step},
    {"otp", HEALTH_BACKGROUND, STATE_PERIOD_S, 50, 0, statecheck_otp_step},
    {"failcount", HEALTH_BACKGROUND, STATE_PERIOD_S, 55, 0, statecheck_failcount_step},
#if SOS_HAVE_SSH
    {"hostkey", HEALTH_BACKGROUND, STATE_PERIOD_S, 60, 0, hostkey_check_step},
#endif
    {"card", HEALTH_BACKGROUND, CARD_PERIOD_S, 90, 0, card_check_step},
#if SOS_HAVE_LUA
    {"core1", HEALTH_BACKGROUND, CORE1_PERIOD_S, CORE1_PERIOD_S, 0, lua_check_core1_step},
#endif
    {"heap-sum", HEALTH_BACKGROUND, HEAPSUM_PERIOD_S, 70, 0, bgcheck_heap_step},
    {"rtc-drift", HEALTH_BACKGROUND, DRIFT_PERIOD_S, 100, 0, bgcheck_drift_step},
#if SOS_HAVE_NET
    {"wifi", HEALTH_BACKGROUND, COUNT_PERIOD_S, 110, 0, wifi_check_step},
#endif
    {"sd-bus", HEALTH_BACKGROUND, COUNT_PERIOD_S, 115, 0, sdbus_check_step},
#if SOS_HAVE_NET
    {"net-pools", HEALTH_BACKGROUND, COUNT_PERIOD_S, 120, 0, pools_check_step},
#endif
    // Phase 5 (§9.7): only on `health full` or `health run <name>`, last, in this order.
    {"ram", HEALTH_FULL, 0, 0, HEALTH_GATE_KEYS | HEALTH_GATE_CRYPTO, fullcheck_ram_step},
    {"image", HEALTH_FULL, 0, 0, HEALTH_GATE_KEYS | HEALTH_GATE_CRYPTO, fullcheck_image_step},
    {"sd-check", HEALTH_FULL, 0, 0, 0, fsck_check_step},
};

void health_boot(void) {
    fullcheck_boot();                   // the image's hash items as the boot ROM just verified them
    health_init(checks, sizeof(checks) / sizeof(checks[0]), time_us_64);
    booting = true;
    health_run_boot();
    booting = false;
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

// `health crypto-times`: every known answer once, each timed, for the board run that measures
// the boot tier's budget (docs/design/health.md §9.2). Holds the main loop for all of them.
void health_board_crypto_times(health_print_fn print, void *arg) {
    char line[96];
    selftest_t *st = selftest_open();
    if (!st) {
        print(arg, "no memory to run the tests");
        return;
    }
    uint32_t boot_us = 0, rest_us = 0;
    for (unsigned i = 0; i < selftest_count(); i++) {
        const selftest_info_t *t = selftest_info(i);
        uint64_t t0 = time_us_64();
        const char *why = selftest_run(st, i);
        uint32_t us = (uint32_t)(time_us_64() - t0);
        *(t->boot ? &boot_us : &rest_us) += us;
        snprintf(line, sizeof(line), "%-12s %-10s %-4s %8u us  %s", t->name,
                 selftest_group_name(t->group), t->boot ? "boot" : "ssh", (unsigned)us,
                 why ? why : "pass");
        print(arg, line);
    }
    selftest_close(st);
    snprintf(line, sizeof(line), "boot subset (crypto) %u us, the rest (crypto-ssh) %u us",
             (unsigned)boot_us, (unsigned)rest_us);
    print(arg, line);
}

// `health trng-diag`: the TRNG under each of a few setups, for the board run to compare with
// the check's own (trng_check_cfg). Each setup collects 16 blocks or stops after 50 ms or 64
// thrown-away blocks.
void health_board_trng_diag(health_print_fn print, void *arg) {
    static const uint32_t samples[] = {17, 64, 256};
    char line[112];
    print(arg, "sample autocorr vn   blocks crngt vn-err autocorr-err run prop     us");
    for (unsigned s = 0; s < sizeof(samples) / sizeof(samples[0]); s++) {
        for (unsigned ac = 0; ac < 2; ac++) {
            for (unsigned vn = 0; vn < 2; vn++) {
                trng_cfg_t cfg = {samples[s], ac != 0, vn != 0};
                hwtest_trng_t t;
                trng_run(&cfg, 50000, 64, &t);
                bool chosen = cfg.sample == trng_check_cfg.sample &&
                              cfg.autocorr == trng_check_cfg.autocorr &&
                              cfg.vn == trng_check_cfg.vn;
                snprintf(line, sizeof(line), "%6u %-8s %-4s %6u %5u %6u %-12s %3u %4u %6u%s",
                         (unsigned)cfg.sample, cfg.autocorr ? "on" : "off", cfg.vn ? "on" : "off",
                         t.blocks, t.crngt_errors, t.vn_errors, t.autocorr_error ? "yes" : "no",
                         (unsigned)t.stats.max_run, (unsigned)t.stats.max_count, (unsigned)t.us,
                         chosen ? "  <- the check's" : "");
                print(arg, line);
            }
        }
    }
    snprintf(line, sizeof(line), "fails at: run %u, prop %u of %u; blocks under %u; crngt over %u",
             HWTEST_TRNG_RCT_CUTOFF, HWTEST_TRNG_APT_CUTOFF, HWTEST_TRNG_APT_WINDOW,
             HWTEST_TRNG_BLOCKS, HWTEST_TRNG_CRNGT_MAX);
    print(arg, line);
}
#endif
