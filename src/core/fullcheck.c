// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See fullcheck.h. Texts stay under HEALTH_TEXT_MAX - 1 characters (tests/test_fullcheck.c
// checks every one it makes).

#include "core/fullcheck.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void set(health_result_t *out, health_status_t status, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void set(health_result_t *out, health_status_t status, const char *fmt, ...) {
    out->status = status;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out->text, sizeof(out->text), fmt, ap);
    va_end(ap);
}

// "0.12 s"
static void seconds(char *buf, size_t cap, uint32_t ms) {
    snprintf(buf, cap, "%lu.%02lu s", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000 / 10));
}

// --- ram -----------------------------------------------------------------------------------------

void fullcheck_judge_ram(const fullcheck_ram_t *in, health_result_t *out) {
    if (in->fault == FULLCHECK_RAM_TWICE) {
        set(out, HEALTH_FAIL, "RAM 0x%08lx bad: power-cycle; keys, seals off until reset",
            (unsigned long)in->addr);
        return;
    }
    if (in->fault == FULLCHECK_RAM_ONCE) {
        set(out, HEALTH_WARN, "0x%08lx read %08lx for %08lx once, not again",
            (unsigned long)in->addr, (unsigned long)in->read, (unsigned long)in->wrote);
        return;
    }
    char t[16];
    seconds(t, sizeof(t), in->ms);
    unsigned kb = (unsigned)(in->heap_bytes / 1024);
    unsigned free_kb = (unsigned)(in->free_before / 1024);
    unsigned reserve_kb = (unsigned)(FULLCHECK_RAM_RESERVE / 1024);
    if (in->heap_bytes && in->scratch_tested)
        set(out, HEALTH_PASS, "%u KB of free heap + 4 KB SCRATCH_Y in %s: no faults", kb, t);
    else if (in->heap_bytes)
        set(out, HEALTH_PASS, "%u KB free heap in %s; %s: not tested", kb, t,
            in->scratch_why ? in->scratch_why : "SCRATCH_Y");
    else if (in->scratch_tested)
        set(out, HEALTH_PASS, "free heap %u KB, under %u KB: SCRATCH_Y only, no faults", free_kb,
            reserve_kb);
    else
        set(out, HEALTH_SKIP, "free heap %u KB, under the %u KB kept free: not tested", free_kb,
            reserve_kb);
}

// --- image ---------------------------------------------------------------------------------------

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool fullcheck_hash_matches(const bootkey_hash_t *h, const uint8_t computed[32]) {
    if (h->value_words == 0 || h->value_words > 8)
        return false;
    for (unsigned i = 0; i < h->value_words; i++)
        if (le32(computed + 4 * i) != h->value[i])
            return false;
    return true;
}

static bool same_value(const bootkey_hash_t *a, const bootkey_hash_t *b) {
    return a->value_words == b->value_words &&
           !memcmp(a->value, b->value, sizeof(a->value[0]) * a->value_words);
}

void fullcheck_judge_image(const fullcheck_image_t *in, health_result_t *out) {
    if (in->boot == BOOTKEY_HASH_NONE && in->now == BOOTKEY_HASH_NONE) {
        set(out, HEALTH_SKIP, "image has no hash (built before phase 5?): not checked");
        return;
    }
    if (in->boot == in->now && in->boot != BOOTKEY_HASH_OK) {
        set(out, HEALTH_WARN, "image's hash block not understood: not checked");
        return;
    }
    if (in->boot != in->now || !same_value(in->at_boot, in->in_flash)) {
        set(out, HEALTH_FAIL, "image hash item changed: reflash; keys, seals off until reset");
        return;
    }
    if (!in->computed || !fullcheck_hash_matches(in->at_boot, in->computed)) {
        set(out, HEALTH_FAIL, "flash image changed: reflash; keys, seals off until reset");
        return;
    }
    char t[16];
    seconds(t, sizeof(t), in->ms);
    unsigned tenths = (unsigned)((in->bytes * 10 + 524288) / 1048576);     // MB, one decimal
    set(out, HEALTH_PASS, "%s, sha256 %02x%02x%02x%02x%02x%02x%02x%02x as booted, %u.%u MB in %s",
        in->in_flash->signed_image ? "signed" : "hashed", in->computed[0], in->computed[1],
        in->computed[2], in->computed[3], in->computed[4], in->computed[5], in->computed[6],
        in->computed[7], tenths / 10, tenths % 10, t);
}

bool fullcheck_hash_next(const bootkey_hash_t *h, fullcheck_hash_cursor_t *c, uint32_t max,
                         fullcheck_piece_t *p) {
    memset(p, 0, sizeof(*p));
    for (;;) {
        if (c->entry > h->loads)
            return false;
        bool block = c->entry == h->loads;
        uint32_t total = block ? h->block_words * 4u : h->load[c->entry].size;
        if (!block && h->load[c->entry].storage == BOOTKEY_ZERO_FILL) {
            // Hashed as the 32-bit size, once.
            p->size_word = true;
            p->size = total;
            c->entry++;
            c->done = 0;
            return true;
        }
        if (c->done >= total) {
            c->entry++;
            c->done = 0;
            continue;
        }
        uint32_t start = block ? (uint32_t)h->block : h->load[c->entry].storage;
        p->off = start + c->done;
        p->len = total - c->done < max ? total - c->done : max;
        c->done += p->len;
        return true;
    }
}

size_t fullcheck_hash_bytes(const bootkey_hash_t *h) {
    size_t n = (size_t)h->block_words * 4;
    for (unsigned i = 0; i < h->loads; i++)
        n += h->load[i].storage == BOOTKEY_ZERO_FILL ? 4 : h->load[i].size;
    return n;
}

// --- sd-check ------------------------------------------------------------------------------------

static void lost_size(char *buf, size_t cap, uint64_t bytes) {
    if (bytes >= 10ull * 1024 * 1024)
        snprintf(buf, cap, "%llu MB", (unsigned long long)(bytes / (1024 * 1024)));
    else
        snprintf(buf, cap, "%llu KB", (unsigned long long)((bytes + 1023) / 1024));
}

void fullcheck_judge_sd(const fullcheck_sd_t *in, health_result_t *out) {
    const fsck_result_t *r = in->res;
    switch (in->state) {
    case FULLCHECK_SD_MOUNTED:
        set(out, HEALTH_SKIP, "card mounted: sd unmount, then health run sd-check");
        return;
    case FULLCHECK_SD_NOT_BY_HAND:
        set(out, HEALTH_SKIP, "not unmounted by hand: sd unmount, then health run sd-check");
        return;
    case FULLCHECK_SD_BUSY:
        set(out, HEALTH_SKIP, "%s", in->why ? in->why : "the card is busy");
        return;
    case FULLCHECK_SD_NOT_OPENED:
        set(out, HEALTH_SKIP, "not checked: %.48s", in->why ? in->why : "?");
        return;
    case FULLCHECK_SD_FAILED:
        if (r->passes)
            set(out, HEALTH_WARN, "check stopped in pass %lu: %.36s", (unsigned long)r->pass,
                r->why ? r->why : "?");
        else
            set(out, HEALTH_WARN, "not checked: %.48s", r->why ? r->why : "?");
        return;
    case FULLCHECK_SD_DONE:
        break;
    }
    unsigned long problems = (unsigned long)r->problems;
    if (r->cross_linked) {
        set(out, HEALTH_FAIL, "%lu cross-linked clusters: sd check fix before writing",
            (unsigned long)r->cross_linked);
        return;
    }
    if (r->reached_free) {
        set(out, HEALTH_FAIL, "%lu clusters used but free: sd check fix before writing",
            (unsigned long)r->reached_free);
        return;
    }
    if (problems) {
        if (r->lost_clusters) {
            char lost[24];
            lost_size(lost, sizeof(lost), (uint64_t)r->lost_clusters * r->cluster_bytes);
            set(out, HEALTH_WARN, "%lu problem%s, %s lost: sd check fix repairs %s", problems,
                problems == 1 ? "" : "s", lost, problems == 1 ? "it" : "them");
        } else {
            set(out, HEALTH_WARN, "%lu problem%s: sd check lists %s, sd check fix repairs",
                problems, problems == 1 ? "" : "s", problems == 1 ? "it" : "them");
        }
        return;
    }
    uint64_t tenths = r->volume_sectors * 10 / (2 * 1024 * 1024);          // GB, one decimal
    unsigned long passes = (unsigned long)r->passes;
    set(out, HEALTH_PASS, "%s%s %llu.%llu GB: no problems, %lu pass%s in %lu.%lu s",
        r->kind == FSCK_KIND_EXFAT ? "exFAT" : "FAT32", in->encrypted ? " encrypted" : "",
        (unsigned long long)(tenths / 10), (unsigned long long)(tenths % 10), passes,
        passes == 1 ? "" : "es", (unsigned long)(in->ms / 1000),
        (unsigned long)(in->ms % 1000 / 100));
}
