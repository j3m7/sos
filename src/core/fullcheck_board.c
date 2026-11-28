// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Phase 5's health checks, the work on the board (docs/design/health.md §9.7): `ram`, a march
// test (core/memtest.h) over heap blocks it allocates and SCRATCH_Y in place; `image`, the
// running image hashed from flash against the HASH_VALUE copied at boot; `sd-check`, `sd check`'s
// job (files/sdcard.h). The judging is in fullcheck.c (pure, host tested). Each step stays under
// 10 ms but the card's opening in `sd-check`'s first step, as every mount's.

#include "core/fullcheck_board.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hardware/regs/addressmap.h"
#include "hardware/structs/dma.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/sha256.h>

#include "core/bootkey.h"
#include "core/fullcheck.h"
#include "core/heap.h"
#include "core/heaptag.h"
#include "core/log.h"
#include "core/memtest.h"

#ifdef SOS_TEST_COMMANDS
static int ram_fault_armed;             // 1: once, 2: twice (health fault ram)
static bool image_fault_armed;          // health fault image
#endif

// --- ram -----------------------------------------------------------------------------------------

// SCRATCH_Y, tested in place. SCRATCH_X, just below it, is the linker's reserved stack for core 1
// (`.stack1_dummy`) and is never tested: SOS starts core 1 on a heap stack today, but a
// multicore_launch_core1() would use it without a word.
#define SCRATCH_BASE SRAM_SCRATCH_Y_BASE
#define SCRATCH_SIZE 4096u
_Static_assert(SCRATCH_BASE >= SRAM_SCRATCH_X_BASE + 4096u ||
               SCRATCH_BASE + SCRATCH_SIZE <= SRAM_SCRATCH_X_BASE,
               "the RAM test's in-place range must not reach SCRATCH_X (core 1's reserved stack)");
_Static_assert(SCRATCH_SIZE % FULLCHECK_RAM_PIECE == 0, "SCRATCH_Y in whole pieces");
#define SCRATCH_PIECES (SCRATCH_SIZE / FULLCHECK_RAM_PIECE)
#define MALLOC_OVERHEAD 16u             // newlib's chunk header, with room

typedef enum { RAM_HEAP, RAM_SCRATCH, RAM_DONE } ram_phase_t;

typedef struct {
    ram_phase_t phase;
    uint64_t t0;
    size_t free_before, heap_bytes;
    void **blocks;                      // the blocks held, released at the end
    unsigned n, cap;
    void *save;                         // a piece's contents while it is tested (no block held)
    unsigned piece;                     // SCRATCH_Y's next piece
    bool scratch_ok;                    // still testable
    const char *scratch_why;
    char why[32];
    // A mismatch: the same region is tested again in the next step.
    bool retest;
    volatile uint32_t *retest_at;       // the region
    size_t retest_words;
    bool retest_in_place;               // a piece of SCRATCH_Y
    memtest_fault_t first;
    uint32_t first_addr;
    int dma;                            // a busy DMA channel writing into the region, or -1
    fullcheck_ram_t result;
} ram_state_t;

// The last mismatch, kept for `health state-dump`.
static struct {
    bool seen;
    uint32_t addr, wrote, read, background;
    uint8_t element;
    int dma;
    bool repeated;
} ram_last;

// A busy DMA channel whose write address is inside [lo, hi), or -1. All NUM_DMA_CHANNELS (16)
// are looked at: CTRL_TRIG.BUSY and WRITE_ADDR.
static int dma_writing(uintptr_t lo, uintptr_t hi) {
    for (int ch = 0; ch < (int)NUM_DMA_CHANNELS; ch++) {
        if (!(dma_hw->ch[ch].ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS))
            continue;
        uintptr_t w = dma_hw->ch[ch].write_addr;
        if (w >= lo && w < hi)
            return ch;
    }
    return -1;
}

static uint32_t msplim(void) {
    uint32_t v;
    __asm volatile("mrs %0, msplim" : "=r"(v));
    return v;
}

// SCRATCH_Y is free when the linker put nothing there and neither core 0's stack nor its guard
// is in it.
static const char *scratch_in_use(void) {
    extern char __scratch_y_start__, __scratch_y_end__, __StackBottom, __StackTop;
    uintptr_t lo = SCRATCH_BASE, hi = SCRATCH_BASE + SCRATCH_SIZE;
    if (&__scratch_y_end__ != &__scratch_y_start__)
        return "SCRATCH_Y in use";
    uintptr_t sb = (uintptr_t)&__StackBottom, st = (uintptr_t)&__StackTop;
    uintptr_t lim = msplim();
    if ((sb < hi && st > lo) || (lim >= lo && lim < hi))
        return "SCRATCH_Y holds the stack";
    return NULL;
}

#ifdef SOS_TEST_COMMANDS
static void ram_disturb(void *arg, volatile uint32_t *words, size_t n) {
    (void)arg;
    words[n / 2] ^= 0x400u;
}
#endif

// Test one region; true if clean. `first`: the first test of it (a fault armed by hand applies to
// the first test, and to the second too when armed twice).
static bool ram_test(volatile uint32_t *words, size_t n, bool first, memtest_fault_t *f) {
    memtest_disturb_fn disturb = NULL;
#ifdef SOS_TEST_COMMANDS
    if (ram_fault_armed && (first || ram_fault_armed == 2))
        disturb = ram_disturb;
    if (!first)
        ram_fault_armed = 0;
#else
    (void)first;
#endif
    return memtest_march(words, n, disturb, NULL, f);
}

// A piece of SCRATCH_Y, in place: its contents saved in `save`, tested with interrupts off on
// core 0, put back. About 0.5 ms.
static bool scratch_test(volatile uint32_t *piece, void *save, bool first, memtest_fault_t *f) {
    size_t words = FULLCHECK_RAM_PIECE / 4;
    uint32_t irq = save_and_disable_interrupts();
    for (size_t i = 0; i < words; i++)
        ((uint32_t *)save)[i] = piece[i];
    bool ok = ram_test(piece, words, first, f);
    for (size_t i = 0; i < words; i++)
        piece[i] = ((uint32_t *)save)[i];
    restore_interrupts(irq);
    return ok;
}

static void ram_free(ram_state_t *st) {
    for (unsigned i = 0; st->blocks && i < st->n; i++)
        heap_release(HEAP_HEALTH, st->blocks[i]);
    heap_release(HEAP_HEALTH, (void *)st->blocks);
    heap_release(HEAP_HEALTH, st->save);
    heap_release(HEAP_HEALTH, st);
}

static void *save_buffer(ram_state_t *st) {
    if (st->n)
        return st->blocks[0];           // tested, and not needed for anything else now
    if (!st->save)
        st->save = heap_alloc(HEAP_HEALTH, FULLCHECK_RAM_PIECE);
    return st->save;
}

// A first mismatch: remember it, and test the region again in the next step.
static void ram_mismatch(ram_state_t *st, volatile uint32_t *at, size_t words, bool in_place,
                         const memtest_fault_t *f) {
    st->retest = true;
    st->retest_at = at;
    st->retest_words = words;
    st->retest_in_place = in_place;
    st->first = *f;
    st->first_addr = (uint32_t)(uintptr_t)&at[f->index];
    st->dma = dma_writing((uintptr_t)at, (uintptr_t)(at + words));
}

// The second test of a region that failed once: FAIL if the same address is wrong again, WARN
// otherwise. Either way it is logged with the details, so two runs can be compared.
static void ram_second(ram_state_t *st) {
    memtest_fault_t f = {0};
    bool ok;
    if (st->retest_in_place) {
        void *save = save_buffer(st);   // held since the first test: there is one
        ok = !save || scratch_test(st->retest_at, save, false, &f);
    } else {
        ok = ram_test(st->retest_at, st->retest_words, false, &f);
    }
    uint32_t addr = ok ? 0 : (uint32_t)(uintptr_t)&st->retest_at[f.index];
    bool repeated = !ok && addr == st->first_addr;
    st->retest = false;
    char dma[32] = "";
    if (st->dma >= 0)
        snprintf(dma, sizeof(dma), "; DMA %d writes there", st->dma);
    log_msg(SOS_LOG_KERN | (repeated ? SOS_LOG_ERR : SOS_LOG_WARNING),
            "health: ram: 0x%08lx wrote %08lx read %08lx (%s, background %08lx)%s; %s",
            (unsigned long)st->first_addr, (unsigned long)st->first.wrote,
            (unsigned long)st->first.read, memtest_element_name(st->first.element),
            (unsigned long)st->first.background, dma,
            // Kept short: the whole line must fit LOG_LINE_MAX (found cut on dev-1).
            repeated ? "repeats: keys, seals off until reset; power-cycle"
                     : "not in a second test");
    ram_last.seen = true;
    ram_last.addr = st->first_addr;
    ram_last.wrote = st->first.wrote;
    ram_last.read = st->first.read;
    ram_last.background = st->first.background;
    ram_last.element = st->first.element;
    ram_last.dma = st->dma;
    ram_last.repeated = repeated;
    if (repeated || st->result.fault == FULLCHECK_RAM_CLEAN) {
        st->result.fault = repeated ? FULLCHECK_RAM_TWICE : FULLCHECK_RAM_ONCE;
        st->result.addr = st->first_addr;
        st->result.wrote = st->first.wrote;
        st->result.read = st->first.read;
    }
    if (repeated)
        st->phase = RAM_DONE;
}

static void ram_heap_step(ram_state_t *st) {
    if (heap_free() < FULLCHECK_RAM_RESERVE + FULLCHECK_RAM_BLOCK + MALLOC_OVERHEAD ||
        st->n == st->cap) {
        st->phase = RAM_SCRATCH;
        return;
    }
    uint32_t *b = heap_alloc(HEAP_HEALTH, FULLCHECK_RAM_BLOCK);
    if (!b) {
        st->phase = RAM_SCRATCH;
        return;
    }
    st->blocks[st->n++] = b;            // kept, so the next block is other memory
    memtest_fault_t f;
    volatile uint32_t *w = b;
    if (!ram_test(w, FULLCHECK_RAM_BLOCK / 4, true, &f))
        ram_mismatch(st, w, FULLCHECK_RAM_BLOCK / 4, false, &f);
    else
        st->heap_bytes += FULLCHECK_RAM_BLOCK;
}

static void ram_scratch_step(ram_state_t *st) {
    if (!st->scratch_ok || st->piece >= SCRATCH_PIECES) {
        st->phase = RAM_DONE;
        return;
    }
    void *save = save_buffer(st);
    int ch = dma_writing(SCRATCH_BASE, SCRATCH_BASE + SCRATCH_SIZE);
    if (!save || ch >= 0) {
        st->scratch_ok = false;
        if (ch >= 0)
            snprintf(st->why, sizeof(st->why), "SCRATCH_Y DMA %d busy", ch);
        else
            snprintf(st->why, sizeof(st->why), "SCRATCH_Y no memory");
        st->scratch_why = st->why;
        st->phase = RAM_DONE;
        return;
    }
    volatile uint32_t *piece = (volatile uint32_t *)(SCRATCH_BASE + st->piece * FULLCHECK_RAM_PIECE);
    st->piece++;
    memtest_fault_t f;
    if (!scratch_test(piece, save, true, &f))
        ram_mismatch(st, piece, FULLCHECK_RAM_PIECE / 4, true, &f);
}

health_step_t fullcheck_ram_step(void **state, bool cancel, health_result_t *out) {
    ram_state_t *st = *state;
    if (cancel) {
        if (st)
            ram_free(st);
        *state = NULL;
        return HEALTH_DONE;
    }
    if (!st) {
        st = heap_zalloc(HEAP_HEALTH, sizeof(*st));
        size_t free_now = heap_free();
        unsigned cap = (unsigned)(free_now / FULLCHECK_RAM_BLOCK) + 1;
        if (st)
            st->blocks = (void **)heap_zalloc(HEAP_HEALTH, cap * sizeof(void *));
        if (!st || !st->blocks) {
            if (st)
                ram_free(st);
            out->status = HEALTH_SKIP;
            snprintf(out->text, sizeof(out->text), "no memory to run the test");
            return HEALTH_DONE;
        }
        *state = st;
        st->cap = cap;
        st->t0 = time_us_64();
        st->free_before = free_now;
        st->scratch_why = scratch_in_use();
        st->scratch_ok = st->scratch_why == NULL;
        return HEALTH_MORE;
    }
    if (st->retest)
        ram_second(st);
    else if (st->phase == RAM_HEAP)
        ram_heap_step(st);
    else if (st->phase == RAM_SCRATCH)
        ram_scratch_step(st);
    if (st->phase != RAM_DONE || st->retest)
        return HEALTH_MORE;
    fullcheck_ram_t *r = &st->result;
    r->heap_bytes = st->heap_bytes;
    r->free_before = st->free_before;
    r->scratch_tested = st->scratch_ok && st->piece == SCRATCH_PIECES;
    r->scratch_why = r->scratch_tested ? NULL : st->scratch_why ? st->scratch_why : "SCRATCH_Y";
    r->ms = (uint32_t)((time_us_64() - st->t0) / 1000);
    fullcheck_judge_ram(r, out);
    ram_free(st);
    *state = NULL;
    return HEALTH_DONE;
}

// --- image ---------------------------------------------------------------------------------------

#define IMAGE_STEP_BYTES 16384u         // hashed a step: about 5 to 6.5 ms

static bootkey_hash_kind_t boot_kind = BOOTKEY_HASH_NO_BLOCKS;
static bootkey_hash_t boot_hash;        // the image's hash items as booted

// The image as `bootkey` finds it, seen through the XIP alias at `alias` (the cached one at
// boot, the uncached no-allocate one for the check): from the start of flash to the block added
// after linking, which lies just past what the linker calls the end.
static void image_view(uintptr_t alias, const uint8_t **image, size_t *len) {
    extern char __flash_binary_start, __flash_binary_end;
    uintptr_t start = (uintptr_t)&__flash_binary_start, end = (uintptr_t)&__flash_binary_end;
    size_t n = (size_t)(end - start) + 4096;
    size_t flash_left = XIP_BASE + PICO_FLASH_SIZE_BYTES - start;
    *image = (const uint8_t *)(alias + (start - XIP_BASE));
    *len = n < flash_left ? n : flash_left;
}

void fullcheck_boot(void) {
    const uint8_t *image;
    size_t len;
    image_view(XIP_BASE, &image, &len);
    boot_kind = bootkey_image_hash(image, len, XIP_BASE, &boot_hash);
}

typedef struct {
    bootkey_hash_kind_t now;
    bootkey_hash_t def;                 // the items in flash now
    fullcheck_hash_cursor_t cur;
    wc_Sha256 sha;
    const uint8_t *image;               // through the uncached, no-allocate alias
    uint64_t t0;
} image_state_t;

static void image_free(void **state) {
    image_state_t *st = *state;
    if (st) {
        wc_Sha256Free(&st->sha);
        heap_release(HEAP_HEALTH, st);
    }
    *state = NULL;
}

static void image_judge(image_state_t *st, const uint8_t *computed, health_result_t *out) {
    fullcheck_image_t in = {
        .boot = boot_kind, .now = st->now, .at_boot = &boot_hash, .in_flash = &st->def,
        .computed = computed, .bytes = fullcheck_hash_bytes(&st->def),
        .ms = (uint32_t)((time_us_64() - st->t0) / 1000),
    };
    fullcheck_judge_image(&in, out);
    if (out->status == HEALTH_FAIL && computed) {
        // Both in byte order, as the PASS text and `picotool info` show a hash (found on dev-1:
        // the booted hash printed as little-endian words looked unrelated to picotool's).
        char booted[17];
        for (unsigned i = 0; i < 8; i++)
            snprintf(booted + 2 * i, 3, "%02x",
                     (unsigned)((boot_hash.value[i / 4] >> (8 * (i % 4))) & 0xffu));
        log_msg(SOS_LOG_KERN | SOS_LOG_ERR,
                "health: image: flash hashes to %02x%02x%02x%02x%02x%02x%02x%02x, booted as %s; "
                "the boot ROM won't start this image after a reset: reflash", computed[0],
                computed[1], computed[2], computed[3], computed[4], computed[5], computed[6],
                computed[7], booted);
    }
}

health_step_t fullcheck_image_step(void **state, bool cancel, health_result_t *out) {
    if (cancel) {
        image_free(state);
        return HEALTH_DONE;
    }
    image_state_t *st = *state;
    if (!st) {
        st = heap_zalloc(HEAP_HEALTH, sizeof(*st));
        if (!st || wc_InitSha256(&st->sha) != 0) {
            heap_release(HEAP_HEALTH, st);
            out->status = HEALTH_SKIP;
            snprintf(out->text, sizeof(out->text), "no memory to hash the image");
            return HEALTH_DONE;
        }
        *state = st;
        st->t0 = time_us_64();
        // The flash itself, not the cache's copy; and 1.25 MB streamed through the 16 KB cache
        // would evict what the main loop runs.
        size_t len;
        image_view(XIP_NOCACHE_NOALLOC_BASE, &st->image, &len);
        st->now = bootkey_image_hash(st->image, len, XIP_BASE, &st->def);
        if (st->now != BOOTKEY_HASH_OK || boot_kind != BOOTKEY_HASH_OK) {
            image_judge(st, NULL, out);
            image_free(state);
            return HEALTH_DONE;
        }
        return HEALTH_MORE;
    }
    uint32_t left = IMAGE_STEP_BYTES;
    fullcheck_piece_t p;
    while (left && fullcheck_hash_next(&st->def, &st->cur, left, &p)) {
        if (p.size_word) {
            uint8_t w[4] = {(uint8_t)p.size, (uint8_t)(p.size >> 8), (uint8_t)(p.size >> 16),
                            (uint8_t)(p.size >> 24)};
            wc_Sha256Update(&st->sha, w, sizeof(w));
            continue;
        }
        const uint8_t *src = st->image + p.off;
#ifdef SOS_TEST_COMMANDS
        if (image_fault_armed && p.len >= 64) {
            uint8_t copy[64];
            memcpy(copy, src, sizeof(copy));
            copy[17] ^= 0x01;
            wc_Sha256Update(&st->sha, copy, sizeof(copy));
            src += sizeof(copy);
            p.len -= sizeof(copy);
            left -= sizeof(copy);
            image_fault_armed = false;
        }
#endif
        wc_Sha256Update(&st->sha, src, p.len);
        left -= p.len;
    }
    if (left == 0)
        return HEALTH_MORE;             // perhaps exactly done: the next step finds nothing left
    uint8_t digest[WC_SHA256_DIGEST_SIZE];
    bool ok = wc_Sha256Final(&st->sha, digest) == 0;
    image_judge(st, ok ? digest : NULL, out);
    image_free(state);
    return HEALTH_DONE;
}

// --- test builds ---------------------------------------------------------------------------------

#ifdef SOS_TEST_COMMANDS
bool fullcheck_board_arm_fault(const char *what, const char *how) {
    if (what && !strcmp(what, "ram") && how && (!strcmp(how, "once") || !strcmp(how, "twice"))) {
        ram_fault_armed = !strcmp(how, "once") ? 1 : 2;
        return true;
    }
    if (what && !strcmp(what, "image") && !how) {
        image_fault_armed = true;
        return true;
    }
    return false;
}

static void dump(health_print_fn print, void *arg, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void dump(health_print_fn print, void *arg, const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    print(arg, line);
}

void fullcheck_board_dump(health_print_fn print, void *arg) {
    if (ram_last.seen)
        dump(print, arg, "ram: last mismatch 0x%08lx wrote %08lx read %08lx, %s, background "
             "%08lx, %s; DMA writing there: %d", (unsigned long)ram_last.addr,
             (unsigned long)ram_last.wrote, (unsigned long)ram_last.read,
             memtest_element_name(ram_last.element), (unsigned long)ram_last.background,
             ram_last.repeated ? "repeated" : "not repeated", ram_last.dma);
    else
        dump(print, arg, "ram: no mismatch since boot; SCRATCH_Y %s",
             scratch_in_use() ? scratch_in_use() : "free");
    static const char *const kinds[] = {"ok", "no hash", "not understood", "no blocks"};
    char head[17];                      // byte order, as `picotool info` and the PASS text
    for (unsigned i = 0; i < 8; i++)
        snprintf(head + 2 * i, 3, "%02x",
                 (unsigned)((boot_hash.value[i / 4] >> (8 * (i % 4))) & 0xffu));
    dump(print, arg, "image at boot: %s, block at 0x%08lx, %s, hash %u words %s..., "
         "%u block words hashed", kinds[boot_kind], (unsigned long)(XIP_BASE + boot_hash.block),
         boot_hash.signed_image ? "signed" : "unsigned", boot_hash.value_words, head,
         (unsigned)boot_hash.block_words);
    for (unsigned i = 0; i < boot_hash.loads; i++) {
        const bootkey_load_t *l = &boot_hash.load[i];
        if (l->storage == BOOTKEY_ZERO_FILL)
            dump(print, arg, "image load map %u: zero fill, %lu bytes", i, (unsigned long)l->size);
        else
            dump(print, arg, "image load map %u: 0x%08lx, %lu bytes", i,
                 (unsigned long)(XIP_BASE + l->storage), (unsigned long)l->size);
    }
}
#endif
