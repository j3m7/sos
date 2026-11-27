// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Unit tests for src/files/fsck_plan.c: what `sd check fix` says about a plan and a repair.

#include <stdint.h>
#include <string.h>

#include "files/fsck_plan.h"
#include "test.h"

#define CHECK_STR(actual, expected) do {                                                    \
        test_checks++;                                                                      \
        if (strcmp((actual), (expected)) != 0) {                                            \
            test_failures++;                                                                \
            fprintf(stderr, "%s:%d: %s:\n  got      \"%s\"\n  expected \"%s\"\n",            \
                    __FILE__, __LINE__, test_current, (actual), (expected));                \
        }                                                                                   \
    } while (0)

// fsck_plan_show()'s lines, joined with newlines.
static char shown[2048];

static void collect(void *ctx, const char *s) {
    (void)ctx;
    CHECK(strlen(s) < FSCK_PLAN_LINE_MAX);
    strncat(shown, s, sizeof(shown) - strlen(shown) - 1);
    strncat(shown, "\n", sizeof(shown) - strlen(shown) - 1);
}

static const char *show(const fsck_plan_t *p, bool problems) {
    shown[0] = '\0';
    fsck_plan_show(p, 32768, problems, collect, NULL);
    return shown;
}

static const char *brief(const fsck_plan_t *p) {
    static char out[256];
    fsck_plan_brief(p, out, sizeof(out));
    return out;
}

TEST(sizes) {
    char s[40];
    fsck_plan_size(s, sizeof(s), 512);
    CHECK_STR(s, "512 bytes");
    fsck_plan_size(s, sizeof(s), 32768);
    CHECK_STR(s, "32 KB");
    fsck_plan_size(s, sizeof(s), 1536 * 1024);
    CHECK_STR(s, "1.5 MB");
}

TEST(nothing_at_all) {
    fsck_plan_t p = {0};
    CHECK_STR(show(&p, false), "");
    CHECK_STR(brief(&p), "nothing");
}

TEST(everything_repaired) {
    fsck_plan_t p = {.lost_clusters = 16, .trims = 2, .trim_clusters = 5, .fat_sectors = 3,
                     .fsinfo = true};
    CHECK_STR(show(&p, true),
              "to repair:\n"
              "  free 16 lost clusters (512 KB)\n"
              "  trim 2 chains longer than their files, freeing 5 clusters (160 KB)\n"
              "  copy 3 sectors of the first FAT over the second\n"
              "  set FSInfo's free count to unknown (counted again at the next mount)\n"
              "Cross-links, broken or short chains and damaged entries are only reported.\n");
    CHECK_STR(brief(&p), "free 16 lost clusters, trim 2 chains (5 clusters), copy 3 FAT "
                         "sectors, FSInfo");
}

TEST(singular) {
    fsck_plan_t p = {.lost_clusters = 1, .trims = 1, .trim_clusters = 1, .fat_sectors = 1,
                     .empty_chains = 1};
    CHECK_STR(show(&p, false),
              "to repair:\n"
              "  free 1 lost cluster (32 KB)\n"
              "  trim 1 chain longer than its file, freeing 1 cluster (32 KB)\n"
              "  copy 1 sector of the first FAT over the second\n"
              "left alone:\n"
              "  1 empty file with clusters (its entry would have to change)\n");
    CHECK_STR(brief(&p), "free 1 lost cluster, trim 1 chain (1 cluster), copy 1 FAT sector; "
                         "left: 1 empty file with clusters");
}

TEST(cross_links_skip_trims) {
    fsck_plan_t p = {.lost_clusters = 4, .trims = 2, .trim_clusters = 7, .cross_links = true};
    CHECK(fsck_plan_writes(&p));
    CHECK_STR(show(&p, true),
              "to repair:\n"
              "  free 4 lost clusters (128 KB)\n"
              "left alone:\n"
              "  trims skipped: cross-linked files, fix those on a PC first\n"
              "  (2 chains longer than their files, 7 clusters)\n"
              "Cross-links, broken or short chains and damaged entries are only reported.\n");
    CHECK_STR(brief(&p), "free 4 lost clusters; skipped: 2 trims (cross-links)");
    // Cross-links without a trim to skip: nothing about trims.
    fsck_plan_t q = {.lost_clusters = 4, .cross_links = true};
    CHECK_STR(show(&q, true),
              "to repair:\n"
              "  free 4 lost clusters (128 KB)\n"
              "Cross-links, broken or short chains and damaged entries are only reported.\n");
}

TEST(unread_tree_skips_frees_and_trims) {
    fsck_plan_t p = {.lost_clusters = 9, .trims = 1, .trim_clusters = 2, .unread = true,
                     .fsinfo = true};
    CHECK(fsck_plan_writes(&p));
    CHECK_STR(show(&p, true),
              "to repair:\n"
              "  set FSInfo's free count to unknown (counted again at the next mount)\n"
              "left alone:\n"
              "  frees and trims skipped: part of the tree wasn't read, so what it holds\n"
              "  looks lost (9 clusters, 1 chain to trim): fix the directories above on a PC "
              "first\n"
              "Cross-links, broken or short chains and damaged entries are only reported.\n");
    CHECK_STR(brief(&p), "FSInfo; skipped: 9 lost, 1 trims (tree not all read)");
    // Nothing to write at all: only what is left alone.
    fsck_plan_t q = {.lost_clusters = 9, .unread = true};
    CHECK(!fsck_plan_writes(&q));
    CHECK_STR(brief(&q), "nothing; skipped: 9 lost, 0 trims (tree not all read)");
}

TEST(only_reported_problems) {
    fsck_plan_t p = {.cross_links = true};
    CHECK(!fsck_plan_writes(&p));
    CHECK_STR(show(&p, true),
              "Cross-links, broken or short chains and damaged entries are only reported.\n");
}

TEST(done_lines) {
    char out[256];
    fsck_plan_t d = {.lost_clusters = 21, .trims = 2, .trim_clusters = 5, .fat_sectors = 3,
                     .fsinfo = true};
    fsck_done_line(&d, 32768, 9, out, sizeof(out));
    CHECK_STR(out, "freed 21 clusters (672 KB), trimmed 2 chains, copied 3 FAT sectors, "
                   "FSInfo's free count set to unknown; 9 sectors written");
    fsck_plan_t f = {.fsinfo = true};
    fsck_done_line(&f, 32768, 1, out, sizeof(out));
    CHECK_STR(out, "FSInfo's free count set to unknown; 1 sector written");
    fsck_plan_t none = {0};
    fsck_done_line(&none, 32768, 0, out, sizeof(out));
    CHECK_STR(out, "nothing written");
    // Cut off, never past the buffer.
    char small[16];
    fsck_done_line(&d, 32768, 9, small, sizeof(small));
    CHECK_EQ(strlen(small), sizeof(small) - 1);
}

TEST(largest_counts_fit) {
    fsck_plan_t p = {.lost_clusters = UINT32_MAX, .trims = UINT32_MAX,
                     .trim_clusters = UINT32_MAX, .empty_chains = UINT32_MAX,
                     .fat_sectors = UINT32_MAX, .fsinfo = true};
    show(&p, true);                         // collect() checks every line's length
    p.cross_links = true;
    show(&p, true);
    p.unread = true;
    show(&p, true);
    CHECK(strlen(brief(&p)) < 160);         // one log line (LOG_LINE_MAX)
}

TEST_MAIN(
    RUN(sizes);
    RUN(nothing_at_all);
    RUN(everything_repaired);
    RUN(singular);
    RUN(cross_links_skip_trims);
    RUN(unread_tree_skips_frees_and_trims);
    RUN(only_reported_problems);
    RUN(done_lines);
    RUN(largest_counts_fit);
)
