// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// See fsck_check.h: the `sd-check` health check's judging, moved from core/fullcheck.c
// (docs/design/build-features.md §4.3). Texts stay under HEALTH_TEXT_MAX - 1 characters
// (tests/test_fullcheck.c checks every one it makes).

#include "files/fsck_check.h"

#include <stdarg.h>
#include <stdio.h>

static void set(health_result_t *out, health_status_t status, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void set(health_result_t *out, health_status_t status, const char *fmt, ...) {
    out->status = status;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out->text, sizeof(out->text), fmt, ap);
    va_end(ap);
}

// --- sd-check ------------------------------------------------------------------------------------

static void lost_size(char *buf, size_t cap, uint64_t bytes) {
    if (bytes >= 10ull * 1024 * 1024)
        snprintf(buf, cap, "%llu MB", (unsigned long long)(bytes / (1024 * 1024)));
    else
        snprintf(buf, cap, "%llu KB", (unsigned long long)((bytes + 1023) / 1024));
}

void fsck_check_judge(const fsck_check_in_t *in, health_result_t *out) {
    const fsck_result_t *r = in->res;
    switch (in->state) {
    case FSCK_CHECK_MOUNTED:
        set(out, HEALTH_SKIP, "card mounted: sd unmount, then health run sd-check");
        return;
    case FSCK_CHECK_NOT_BY_HAND:
        set(out, HEALTH_SKIP, "not unmounted by hand: sd unmount, then health run sd-check");
        return;
    case FSCK_CHECK_BUSY:
        set(out, HEALTH_SKIP, "%s", in->why ? in->why : "the card is busy");
        return;
    case FSCK_CHECK_NOT_OPENED:
        set(out, HEALTH_SKIP, "not checked: %.48s", in->why ? in->why : "?");
        return;
    case FSCK_CHECK_FAILED:
        if (r->passes)
            set(out, HEALTH_WARN, "check stopped in pass %lu: %.36s", (unsigned long)r->pass,
                r->why ? r->why : "?");
        else
            set(out, HEALTH_WARN, "not checked: %.48s", r->why ? r->why : "?");
        return;
    case FSCK_CHECK_DONE:
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
