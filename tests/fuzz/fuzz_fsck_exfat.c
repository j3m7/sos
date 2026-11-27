// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// src/files/fsck_exfat.c: what a card's exFAT structures hold, read by the checker's exFAT
// parts. The first byte picks the part (its value modulo 4):
//   0  a boot region: [volume lba: u8][device sectors: u32] then sectors 0-11 (zeros after
//      the input's end); sectors 12-23 are a copy of them, XORed with what follows (so a
//      short input has a backup that matches), through fsck_exfat_vbr() and fsck_exfat_boot_*()
//   1  a directory: [flags: u8 (bit 0: the root)][clusters: u32][cluster shift: u8] then
//      32-byte entries, through fsck_exfat_entry() and fsck_exfat_dir_end()
//   2  an up-case table: [length: u32][chunk: u8] then the table, fed `chunk` bytes at a time
//   3  a window against the bitmap: [first cluster: u32][bits: u16] then the reached bits and
//      the bitmap's, half the rest each
// Every report line is formatted, and the results must keep the header's promises.

#include <stdlib.h>
#include <string.h>

#include "files/fsck_exfat.h"
#include "fuzz.h"

// Checked in every build (NDEBUG or not).
#define MUST(x) do { if (!(x)) abort(); } while (0)

static uint32_t take(const uint8_t **data, size_t *size, unsigned bytes) {
    uint32_t v = 0;
    for (unsigned i = 0; i < bytes; i++)
        v |= (uint32_t)fuzz_byte(data, size) << (8 * i);
    return v;
}

static uint32_t ascii_upcase(uint32_t c) {
    return c >= 'a' && c <= 'z' ? c - 32 : c;
}

static void texts_set(const fsck_exfat_set_t *s, const fsck_exfat_geom_t *g) {
    char out[FSCK_EXFAT_TEXT_MAX];
    for (uint32_t bit = 1; bit; bit <<= 1)
        if (s->problems & bit) {
            fsck_exfat_set_text(s, bit, out, sizeof(out));
            MUST(strlen(out) < sizeof(out));
        }
    MUST(s->name_have <= s->name_len);
    MUST(s->clusters <= g->clusters);
    MUST(s->run_clusters <= s->clusters);
    if (s->run_clusters)
        MUST(s->no_fat_chain && s->first_cluster >= 2 &&
             (uint64_t)s->first_cluster + s->run_clusters <= (uint64_t)g->clusters + 2);
    if (s->kind == FSCK_EXFAT_FILE && s->seen < s->secondaries)
        MUST(s->problems & FSCK_EXFAT_S_SHORT);
}

static void texts_dir(const fsck_exfat_dir_t *d) {
    char out[FSCK_EXFAT_TEXT_MAX];
    for (uint32_t bit = 1; bit; bit <<= 1)
        if (d->problems & bit)
            fsck_exfat_dir_text(d, bit, out, sizeof(out));
}

static void boot(const uint8_t *data, size_t size) {
    uint64_t lba = fuzz_byte(&data, &size);
    uint64_t dev = take(&data, &size, 4);
    static uint8_t sec[24][512];
    memset(sec, 0, sizeof(sec));
    memcpy(sec, data, size < sizeof(sec) ? size : sizeof(sec));
    for (size_t i = 0; i < sizeof(sec) / 2; i++)
        sec[12 + i / 512][i % 512] ^= sec[i / 512][i % 512];
    fsck_exfat_geom_t g;
    const char *why = fsck_exfat_vbr(sec[0], lba, dev, &g);
    if (!why) {
        MUST(g.clusters && g.root_cluster >= 2 && g.root_cluster <= g.clusters + 1);
        MUST(g.volume_lba + g.volume_sectors <= dev);
        MUST((uint64_t)g.heap_offset + (uint64_t)g.clusters * g.cluster_sectors <=
             g.volume_sectors);
        MUST(g.cluster_bytes == g.cluster_sectors * 512);
    }
    fsck_exfat_boot_t b;
    fsck_exfat_boot_init(&b);
    for (int i = 0; i < 24; i++)
        fsck_exfat_boot_sector(&b, sec[i]);
    char out[FSCK_EXFAT_TEXT_MAX];
    uint32_t p = fsck_exfat_boot_done(&b);
    MUST(!(p & ~(FSCK_EXFAT_B_CHECKSUM | FSCK_EXFAT_B_BACKUP)));
    for (uint32_t bit = 1; bit <= p; bit <<= 1)
        if (p & bit)
            fsck_exfat_boot_text(&b, bit, out, sizeof(out));
}

static void dir(const uint8_t *data, size_t size) {
    uint8_t flags = fuzz_byte(&data, &size);
    fsck_exfat_geom_t g = {0};
    g.clusters = take(&data, &size, 4) % 0x7FFFFFFDu + 1;
    unsigned shift = fuzz_byte(&data, &size) % 16;
    g.cluster_sectors = 1u << shift;
    g.cluster_bytes = g.cluster_sectors * 512;
    g.root_cluster = 2;
    fsck_exfat_dir_t d;
    fsck_exfat_dir_begin(&d, &g, ascii_upcase, flags & 1);
    while (size >= 32) {
        uint32_t r = fsck_exfat_entry(&d, data);
        if (r & FSCK_EXFAT_SET)
            texts_set(&d.set, &g);
        if (r & FSCK_EXFAT_DIR)
            texts_dir(&d);
        if (r & FSCK_EXFAT_AGAIN) {         // the same entry again: never cut short twice
            r = fsck_exfat_entry(&d, data);
            MUST(!(r & FSCK_EXFAT_AGAIN));
            if (r & FSCK_EXFAT_SET)
                texts_set(&d.set, &g);
            if (r & FSCK_EXFAT_DIR)
                texts_dir(&d);
        }
        data += 32;
        size -= 32;
    }
    uint32_t r = fsck_exfat_dir_end(&d);
    if (r & FSCK_EXFAT_SET)
        texts_set(&d.set, &g);
    if (r & FSCK_EXFAT_DIR)
        texts_dir(&d);
}

static void upcase(const uint8_t *data, size_t size) {
    uint32_t length = take(&data, &size, 4) % 140000;
    size_t chunk = fuzz_byte(&data, &size) + 1u;
    fsck_exfat_upcase_t u;
    fsck_exfat_upcase_init(&u, length, ascii_upcase);
    for (size_t at = 0; at < size; at += chunk)
        fsck_exfat_upcase_feed(&u, data + at, size - at < chunk ? size - at : chunk);
    MUST(u.fed <= length && u.fed <= size);
    uint32_t r = fsck_exfat_upcase_done(&u, 0);
    MUST(u.code == 0x10000);
    char out[FSCK_EXFAT_TEXT_MAX];
    for (uint32_t bit = 1; bit <= r; bit <<= 1)
        if (r & bit)
            fsck_exfat_upcase_text(&u, bit, out, sizeof(out));
}

static void window(const uint8_t *data, size_t size) {
    uint32_t first = take(&data, &size, 4) % 0x7FFF0000u + 2;
    uint32_t want = take(&data, &size, 2);
    size_t half = size / 2;
    uint32_t bits = (uint32_t)(half * 8 < want ? half * 8 : want);
    const uint8_t *reached = data, *bitmap = data + half;
    fsck_bitcmp_t c;
    fsck_bitcmp_init(&c);
    uint64_t lost = 0, freed = 0, end = first;
    fsck_run_t run;
    int kind;
    // Two slices, the second starting where the first ends.
    uint32_t split = bits / 16 * 8;
    fsck_bitcmp_slice(&c, reached, bitmap, first, split);
    while ((kind = fsck_bitcmp_next(&c, &run))) {
        MUST(run.count && run.first >= first && (uint64_t)run.first + run.count <= first + split);
        *(kind == FSCK_RUN_LOST ? &lost : &freed) += run.count;
    }
    fsck_bitcmp_slice(&c, reached + split / 8, bitmap + split / 8, first + split, bits - split);
    while ((kind = fsck_bitcmp_next(&c, &run))) {
        MUST(run.count && run.first >= first && (uint64_t)run.first + run.count <= first + bits);
        *(kind == FSCK_RUN_LOST ? &lost : &freed) += run.count;
        end = run.first + run.count;
    }
    while ((kind = fsck_bitcmp_finish(&c, &run)))
        *(kind == FSCK_RUN_LOST ? &lost : &freed) += run.count;
    MUST(lost == c.lost_total && freed == c.free_total);
    MUST(end <= (uint64_t)first + bits);
    // Marking the reached bits' runs again into a copy counts each set bit as a cross-link.
    uint8_t win[1024];
    size_t wbytes = bits / 8 < sizeof(win) ? bits / 8 : sizeof(win);
    memcpy(win, reached, wbytes);
    uint32_t cross = 0, set = 0;
    for (size_t i = 0; i < wbytes * 8; i++)
        set += (uint32_t)(reached[i / 8] >> (i % 8)) & 1u;
    uint32_t wbits = (uint32_t)wbytes * 8;
    MUST(fsck_window_mark_run(win, first, wbits, first - 1, wbits + 2, &cross) == set);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    switch (fuzz_byte(&data, &size) % 4) {
    case 0:
        boot(data, size);
        break;
    case 1:
        dir(data, size);
        break;
    case 2:
        upcase(data, size);
        break;
    default:
        window(data, size);
        break;
    }
    return 0;
}
