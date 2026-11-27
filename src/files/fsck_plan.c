// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The words of `sd check fix` (fsck_plan.h).

#include "files/fsck_plan.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void fsck_plan_size(char *out, size_t n, uint64_t bytes) {
    static const char *const units[] = {"bytes", "KB", "MB", "GB", "TB"};
    unsigned u = 0;
    uint64_t div = 1;
    while (u < 4 && bytes >= div * 1024)
        div *= 1024, u++;
    uint64_t whole = bytes / div, tenth = (bytes % div) * 10 / div;
    if (tenth)
        snprintf(out, n, "%" PRIu64 ".%" PRIu64 " %s", whole, tenth, units[u]);
    else
        snprintf(out, n, "%" PRIu64 " %s", whole, units[u]);
}

static const char *plural(uint32_t n, const char *one, const char *many) {
    return n == 1 ? one : many;
}

// Append to `out` (`n` bytes, NUL-terminated), cut off at the end.
__attribute__((format(printf, 3, 4)))
static void add(char *out, size_t n, const char *fmt, ...) {
    size_t len = strnlen(out, n);
    if (len + 1 >= n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + len, n - len, fmt, ap);
    va_end(ap);
}

// What the plan does, kind by kind (fsck_plan_writes()'s rules).
static bool frees(const fsck_plan_t *p) {
    return !p->unread && p->lost_clusters;
}

static bool trims(const fsck_plan_t *p) {
    return !p->unread && !p->cross_links && p->trims;
}

void fsck_plan_show(const fsck_plan_t *p, uint32_t cluster_bytes, bool problems,
                    void (*line)(void *ctx, const char *s), void *ctx) {
    char s[FSCK_PLAN_LINE_MAX], size[40];
    if (fsck_plan_writes(p)) {
        line(ctx, "to repair:");
        if (frees(p)) {
            fsck_plan_size(size, sizeof(size), (uint64_t)p->lost_clusters * cluster_bytes);
            snprintf(s, sizeof(s), "  free %" PRIu32 " lost cluster%s (%s)", p->lost_clusters,
                     plural(p->lost_clusters, "", "s"), size);
            line(ctx, s);
        }
        if (trims(p)) {
            fsck_plan_size(size, sizeof(size), (uint64_t)p->trim_clusters * cluster_bytes);
            snprintf(s, sizeof(s), "  trim %" PRIu32 " chain%s longer than %s file%s, freeing "
                     "%" PRIu32 " cluster%s (%s)", p->trims, plural(p->trims, "", "s"),
                     plural(p->trims, "its", "their"), plural(p->trims, "", "s"),
                     p->trim_clusters, plural(p->trim_clusters, "", "s"), size);
            line(ctx, s);
        }
        if (p->fat_sectors) {
            snprintf(s, sizeof(s), "  copy %" PRIu32 " sector%s of the first FAT over the second",
                     p->fat_sectors, plural(p->fat_sectors, "", "s"));
            line(ctx, s);
        }
        if (p->fsinfo)
            line(ctx, "  set FSInfo's free count to unknown (counted again at the next mount)");
    }
    bool skip_unread = p->unread && (p->lost_clusters || p->trims);
    bool skip_cross = !p->unread && p->cross_links && p->trims;
    if (skip_unread || skip_cross || p->empty_chains) {
        line(ctx, "left alone:");
        if (skip_unread) {
            line(ctx, "  frees and trims skipped: part of the tree wasn't read, so what it holds");
            snprintf(s, sizeof(s), "  looks lost (%" PRIu32 " cluster%s, %" PRIu32 " chain%s to "
                     "trim): fix the directories above on a PC first", p->lost_clusters,
                     plural(p->lost_clusters, "", "s"), p->trims, plural(p->trims, "", "s"));
            line(ctx, s);
        }
        if (skip_cross) {
            line(ctx, "  trims skipped: cross-linked files, fix those on a PC first");
            snprintf(s, sizeof(s), "  (%" PRIu32 " chain%s longer than %s file%s, %" PRIu32
                     " cluster%s)", p->trims, plural(p->trims, "", "s"),
                     plural(p->trims, "its", "their"), plural(p->trims, "", "s"),
                     p->trim_clusters, plural(p->trim_clusters, "", "s"));
            line(ctx, s);
        }
        if (p->empty_chains) {
            snprintf(s, sizeof(s), "  %" PRIu32 " empty file%s with clusters (%s entry would "
                     "have to change)", p->empty_chains, plural(p->empty_chains, "", "s"),
                     plural(p->empty_chains, "its", "their"));
            line(ctx, s);
        }
    }
    if (problems)
        line(ctx, "Cross-links, broken or short chains and damaged entries are only reported.");
}

void fsck_plan_brief(const fsck_plan_t *p, char *out, size_t n) {
    if (!n)
        return;
    out[0] = '\0';
    const char *sep = "";
    if (frees(p)) {
        add(out, n, "free %" PRIu32 " lost cluster%s", p->lost_clusters,
            plural(p->lost_clusters, "", "s"));
        sep = ", ";
    }
    if (trims(p)) {
        add(out, n, "%strim %" PRIu32 " chain%s (%" PRIu32 " cluster%s)", sep, p->trims,
            plural(p->trims, "", "s"), p->trim_clusters, plural(p->trim_clusters, "", "s"));
        sep = ", ";
    }
    if (p->fat_sectors) {
        add(out, n, "%scopy %" PRIu32 " FAT sector%s", sep, p->fat_sectors,
            plural(p->fat_sectors, "", "s"));
        sep = ", ";
    }
    if (p->fsinfo) {
        add(out, n, "%sFSInfo", sep);
        sep = ", ";
    }
    if (!*sep)
        add(out, n, "nothing");
    bool skip_unread = p->unread && (p->lost_clusters || p->trims);
    bool skip_cross = !p->unread && p->cross_links && p->trims;
    if (skip_unread)
        add(out, n, "; skipped: %" PRIu32 " lost, %" PRIu32 " trims (tree not all read)",
            p->lost_clusters, p->trims);
    else if (skip_cross)
        add(out, n, "; skipped: %" PRIu32 " trims (cross-links)", p->trims);
    if (p->empty_chains)
        add(out, n, "; left: %" PRIu32 " empty file%s with clusters", p->empty_chains,
            plural(p->empty_chains, "", "s"));
}

void fsck_done_line(const fsck_plan_t *d, uint32_t cluster_bytes, uint32_t writes, char *out,
                    size_t n) {
    if (!n)
        return;
    out[0] = '\0';
    if (!writes) {
        add(out, n, "nothing written");
        return;
    }
    const char *sep = "";
    if (d->lost_clusters) {
        char size[40];
        fsck_plan_size(size, sizeof(size), (uint64_t)d->lost_clusters * cluster_bytes);
        add(out, n, "freed %" PRIu32 " cluster%s (%s)", d->lost_clusters,
            plural(d->lost_clusters, "", "s"), size);
        sep = ", ";
    }
    if (d->trims) {
        add(out, n, "%strimmed %" PRIu32 " chain%s", sep, d->trims, plural(d->trims, "", "s"));
        sep = ", ";
    }
    if (d->fat_sectors) {
        add(out, n, "%scopied %" PRIu32 " FAT sector%s", sep, d->fat_sectors,
            plural(d->fat_sectors, "", "s"));
        sep = ", ";
    }
    if (d->fsinfo)
        add(out, n, "%sFSInfo's free count set to unknown", sep);
    add(out, n, "%s%" PRIu32 " sector%s written", *out ? "; " : "", writes,
        plural(writes, "", "s"));
}
