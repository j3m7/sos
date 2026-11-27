// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The pure half of the boot tier's hardware checks (core/health.h, docs/design/health.md §10):
// the TRNG's startup health tests, and the judging of the `trng`, `clocks` and `flash` checks.
// health_board.c reads the chip and hands the numbers here; tests/test_hwtest.c runs these on the
// host with made-up readings.

#ifndef SOS_HWTEST_H
#define SOS_HWTEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/health.h"

// --- trng: SP 800-90B §4.4 health tests as a startup test --------------------------------------
//
// Over raw bits from the TRNG's ring oscillator (one bit per sample, the von Neumann corrector
// and the block's autocorrelation test bypassed, its CRNGT on), assuming a min-entropy of
// H = 0.5 bit per sample and a false positive rate of alpha = 2^-30 per test:
// - Repetition Count Test (§4.4.1): C = 1 + ceil(-log2(alpha) / H) = 1 + ceil(30 / 0.5) = 61.
//   A run of 61 identical samples fails.
// - Adaptive Proportion Test (§4.4.2), binary samples, window W = 1024:
//   C = 1 + CRITBINOM(W, 2^-H, 1 - alpha) = 1 + CRITBINOM(1024, 0.7071, 1 - 2^-30) = 810.
//   A window whose first sample occurs 810 or more times in it fails.
// tests/test_hwtest.c computes both cutoffs again from the formulas.
#define HWTEST_TRNG_RCT_CUTOFF  61
#define HWTEST_TRNG_APT_WINDOW  1024
#define HWTEST_TRNG_APT_CUTOFF  810
#define HWTEST_TRNG_BLOCK_WORDS 6       // one EHR block: 192 bits
#define HWTEST_TRNG_BLOCKS      16      // 3072 bits: three APT windows
#define HWTEST_TRNG_WORDS       (HWTEST_TRNG_BLOCKS * HWTEST_TRNG_BLOCK_WORDS)
// The TRNG's own repetition test (CRNGT: two equal 16-bit blocks in a row) throws a block away
// now and then on a good source; more than this many in one run fails.
#define HWTEST_TRNG_CRNGT_MAX   8

typedef struct {
    uint32_t bits;                      // samples tested
    uint32_t max_run;                   // the longest run of identical samples (RCT)
    uint32_t max_count;                 // the most occurrences of a window's first sample (APT)
    uint32_t windows;                   // complete APT windows
} hwtest_trng_stats_t;

// Both tests over n words of samples, each word's bits taken from bit 0 up, words in order.
void hwtest_trng_stats(const uint32_t *words, size_t n, hwtest_trng_stats_t *out);

typedef struct {
    hwtest_trng_stats_t stats;          // over the blocks collected
    unsigned blocks;                    // blocks collected (HWTEST_TRNG_BLOCKS when complete)
    unsigned crngt_errors;              // blocks the TRNG's own repetition test threw away
    unsigned vn_errors;                 // blocks its von Neumann corrector threw away (when on)
    bool autocorr_error;                // the TRNG's own autocorrelation test stopped it
    uint32_t us;                        // how long it took
} hwtest_trng_t;

// FAIL on the TRNG's own autocorrelation error, more than HWTEST_TRNG_CRNGT_MAX of its CRNGT and
// von Neumann errors together, fewer blocks than asked for (it stopped producing), or either
// startup test. The text says which, and after how many blocks.
void hwtest_judge_trng(const hwtest_trng_t *t, health_result_t *out);

// --- clocks ----------------------------------------------------------------------------------
//
// Each clock measured by the frequency counter (FC0) against its configured frequency. FC0
// counts against clk_ref, which comes from the crystal: this sees a PLL or divider set wrong,
// not a crystal that is off. The low-power oscillator (LPOSC, an RC oscillator, 32.768 kHz
// nominal) measured the same way is the cross-check on the crystal: only a gross error shows
// (a crystal off by about 2x), as its own tolerance is wide.
#define HWTEST_CLOCK_WARN_PPM 10000     // 1 %
#define HWTEST_CLOCK_FAIL_PPM 50000     // 5 %
// LPOSC: ±20 % untrimmed, ±14 % over temperature and ±20 % over supply (RP2350 datasheet §8.4.1):
// 26.2 x 0.86 x 0.8 = 18.0 kHz to 39.3 x 1.14 x 1.2 = 53.8 kHz.
#define HWTEST_LPOSC_MIN_KHZ 18
#define HWTEST_LPOSC_MAX_KHZ 54

typedef struct {
    const char *name;                   // "clk_sys"
    uint32_t want_hz;                   // configured (clock_get_hz); 0: not running, skipped
    uint32_t got_khz;                   // measured (frequency_count_khz)
} hwtest_clock_t;

// WARN beyond HWTEST_CLOCK_WARN_PPM, FAIL beyond HWTEST_CLOCK_FAIL_PPM (the text names the
// worst); WARN if the LPOSC is outside its band. clocks[0] is named in a passing result.
void hwtest_judge_clocks(const hwtest_clock_t *clocks, unsigned n, uint32_t lposc_khz,
                         health_result_t *out);

// --- flash -----------------------------------------------------------------------------------

// The JEDEC ID (manufacturer, type, capacity code: 2^code bytes) against the flash size the
// build assumes (PICO_FLASH_SIZE_BYTES). FAIL if smaller (the configuration sectors at its end
// would be past the chip's), WARN on an unknown manufacturer or capacity code, or no answer.
void hwtest_judge_flash(const uint8_t id[3], uint32_t build_bytes, health_result_t *out);

#endif
