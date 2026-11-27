// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The boot tier's hardware checks, the pure half (core/hwtest.h): pure C, host tested
// (tests/test_hwtest.c).

#include "core/hwtest.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// The result's status and text. Its line is cut to fit, as any text is.
__attribute__((format(printf, 3, 4)))
static void result(health_result_t *out, health_status_t s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out->text, sizeof(out->text), fmt, ap);
    va_end(ap);
    out->status = s;
}

// --- trng ------------------------------------------------------------------------------------

void hwtest_trng_stats(const uint32_t *words, size_t n, hwtest_trng_stats_t *out) {
    memset(out, 0, sizeof(*out));
    unsigned last = 2, run = 0;         // RCT: the sample repeating, and how often so far
    unsigned first = 0, count = 0, in_window = 0;   // APT: the window's first sample, its count
    for (size_t i = 0; i < n; i++) {
        for (unsigned b = 0; b < 32; b++) {
            unsigned s = (words[i] >> b) & 1u;
            out->bits++;
            // Repetition Count Test (SP 800-90B §4.4.1).
            run = s == last ? run + 1 : 1;
            last = s;
            if (run > out->max_run)
                out->max_run = run;
            // Adaptive Proportion Test (§4.4.2), over non-overlapping windows.
            if (in_window == 0) {
                first = s;
                count = 0;
            }
            if (s == first)
                count++;
            if (++in_window == HWTEST_TRNG_APT_WINDOW) {
                if (count > out->max_count)
                    out->max_count = count;
                out->windows++;
                in_window = 0;
            }
        }
    }
}

void hwtest_judge_trng(const hwtest_trng_t *t, health_result_t *out) {
    const hwtest_trng_stats_t *s = &t->stats;
    if (t->autocorr_error) {
        result(out, HEALTH_FAIL, "the TRNG's own autocorrelation test failed");
    } else if (t->crngt_errors > HWTEST_TRNG_CRNGT_MAX) {
        result(out, HEALTH_FAIL, "the TRNG's own repetition test failed %u times",
               t->crngt_errors);
    } else if (t->blocks < HWTEST_TRNG_BLOCKS) {
        result(out, HEALTH_FAIL, "the TRNG gave %u of %u blocks in %u ms", t->blocks,
               HWTEST_TRNG_BLOCKS, (unsigned)(t->us / 1000u));
    } else if (s->max_run >= HWTEST_TRNG_RCT_CUTOFF) {
        result(out, HEALTH_FAIL, "repetition count: run of %u, fails at %u",
               (unsigned)s->max_run, HWTEST_TRNG_RCT_CUTOFF);
    } else if (s->max_count >= HWTEST_TRNG_APT_CUTOFF) {
        result(out, HEALTH_FAIL, "adaptive proportion: %u of %u, fails at %u",
               (unsigned)s->max_count, HWTEST_TRNG_APT_WINDOW, HWTEST_TRNG_APT_CUTOFF);
    } else {
        // "run 12 (fails at 61), proportion 545/1024 (810), 1.3 ms"
        unsigned tenths = (unsigned)((t->us + 50u) / 100u);
        result(out, HEALTH_PASS, "run %u (fails at %u), proportion %u/%u (%u), %u.%u ms",
               (unsigned)s->max_run, HWTEST_TRNG_RCT_CUTOFF, (unsigned)s->max_count,
               HWTEST_TRNG_APT_WINDOW, HWTEST_TRNG_APT_CUTOFF, tenths / 10, tenths % 10);
    }
}

// --- clocks ----------------------------------------------------------------------------------

// Parts per million off the configured frequency, signed.
static int64_t off_ppm(const hwtest_clock_t *c) {
    int64_t got = (int64_t)c->got_khz * 1000, want = c->want_hz;
    return (got - want) * 1000000 / want;
}

static int64_t magnitude(int64_t v) {
    return v < 0 ? -v : v;
}

// "150.0": a frequency in MHz with one decimal, from kHz or Hz.
static void mhz(char *out, size_t cap, uint32_t khz) {
    uint32_t tenths = (khz + 50u) / 100u;
    snprintf(out, cap, "%u.%u", (unsigned)(tenths / 10u), (unsigned)(tenths % 10u));
}

void hwtest_judge_clocks(const hwtest_clock_t *clocks, unsigned n, uint32_t lposc_khz,
                         health_result_t *out) {
    int worst = -1;
    unsigned measured = 0;
    int64_t worst_ppm = 0;
    for (unsigned i = 0; i < n; i++) {
        if (!clocks[i].want_hz)
            continue;
        measured++;
        int64_t ppm = off_ppm(&clocks[i]);
        if (worst < 0 || magnitude(ppm) > magnitude(worst_ppm)) {
            worst = (int)i;
            worst_ppm = ppm;
        }
    }
    if (worst < 0) {
        result(out, HEALTH_SKIP, "no clock to measure");
        return;
    }
    int64_t mag = magnitude(worst_ppm);
    if (mag > HWTEST_CLOCK_WARN_PPM) {
        // "clk_usb 47.2 MHz, set to 48.0 (-1.67%)"
        const hwtest_clock_t *c = &clocks[worst];
        char got[16], want[16];
        mhz(got, sizeof(got), c->got_khz);
        mhz(want, sizeof(want), (c->want_hz + 500u) / 1000u);
        unsigned hundredths = (unsigned)((mag + 50) / 100);
        result(out, mag > HWTEST_CLOCK_FAIL_PPM ? HEALTH_FAIL : HEALTH_WARN,
               "%s %s MHz, set to %s (%c%u.%02u%%)", c->name, got, want,
               worst_ppm < 0 ? '-' : '+', hundredths / 100, hundredths % 100);
        return;
    }
    if (lposc_khz < HWTEST_LPOSC_MIN_KHZ || lposc_khz > HWTEST_LPOSC_MAX_KHZ) {
        result(out, HEALTH_WARN, "lposc %u kHz, not %u-%u: crystal wrong?", (unsigned)lposc_khz,
               HWTEST_LPOSC_MIN_KHZ, HWTEST_LPOSC_MAX_KHZ);
        return;
    }
    // "clk_sys 150.0 MHz; 6 clocks within 0.01%; lposc 32 kHz"
    char first[16];
    unsigned hundredths = (unsigned)((mag + 99) / 100);     // rounded up: "within"
    mhz(first, sizeof(first), clocks[0].got_khz);
    result(out, HEALTH_PASS, "%s %s MHz; %u clocks within %u.%02u%%; lposc %u kHz",
           clocks[0].name, first, measured, hundredths / 100, hundredths % 100,
           (unsigned)lposc_khz);
}

// --- flash -----------------------------------------------------------------------------------

static const char *maker(uint8_t id) {
    switch (id) {
    case 0x01: return "Infineon";       // Spansion, Cypress
    case 0x0b: return "XTX";
    case 0x1f: return "Adesto";
    case 0x20: return "Micron";         // also XMC
    case 0x5e: return "Zbit";
    case 0x68: return "Boya";
    case 0x85: return "Puya";
    case 0x9d: return "ISSI";
    case 0xc2: return "Macronix";
    case 0xc8: return "GigaDevice";
    case 0xef: return "Winbond";
    }
    return NULL;
}

// "4 MB", "512 KB".
static void size_text(char *out, size_t cap, uint32_t bytes) {
    if (bytes >= 1024u * 1024u)
        snprintf(out, cap, "%u MB", (unsigned)(bytes / (1024u * 1024u)));
    else
        snprintf(out, cap, "%u KB", (unsigned)(bytes / 1024u));
}

void hwtest_judge_flash(const uint8_t id[3], uint32_t build_bytes, health_result_t *out) {
    char hex[12], want[12];
    snprintf(hex, sizeof(hex), "%02x %02x %02x", id[0], id[1], id[2]);
    size_text(want, sizeof(want), build_bytes);
    if ((id[0] == 0xff && id[1] == 0xff && id[2] == 0xff) || (!id[0] && !id[1] && !id[2])) {
        result(out, HEALTH_WARN, "no JEDEC ID (%s)", hex);
        return;
    }
    const char *who = maker(id[0]);
    // 2^code bytes: 64 KB (0x10) to 2 GB (0x1f) are the codes in use.
    if (id[2] < 0x10 || id[2] > 0x1f) {
        result(out, HEALTH_WARN, "JEDEC %s (%s, size code unknown)", hex,
               who ? who : "unknown maker");
        return;
    }
    uint32_t bytes = 1u << id[2];
    char have[12];
    size_text(have, sizeof(have), bytes);
    if (bytes < build_bytes) {
        result(out, HEALTH_FAIL, "JEDEC %s (%s, %s): the build needs %s", hex,
               who ? who : "unknown maker", have, want);
    } else {
        result(out, who ? HEALTH_PASS : HEALTH_WARN, "JEDEC %s (%s, %s)", hex,
               who ? who : "unknown maker", have);
    }
}
