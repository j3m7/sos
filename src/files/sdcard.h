// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// microSD card on the PiCowbell Adalogger (exploratory).

#ifndef SOS_SDCARD_H
#define SOS_SDCARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "files/fsck.h"
#include "shell/shell.h"

// `sd [info|test [KB [keep]]|speed [MHz]|format [exfat|fat32] [plain]|unmount|check [fix]]`;
// test, speed <MHz>, format, unmount and check need an admin channel. While `sd check` or
// `sd check fix` runs (docs/design/sd-check.md), its question included, the others refuse.
void sdcard_command(shell_t *sh, char *args);

// Mount the card if it isn't yet (initializing the driver first). Says why on failure: to `sh`,
// or in the log when `sh` is NULL. Never while `sd check` runs: false, without touching the card
// (saying so to `sh`).
bool sdcard_mount(shell_t *sh);

// Mount the card if it isn't, quietly (no log lines if there's none): for retrying every so
// often (the log files do). Not after `sd unmount` or `sd check`, until something mounts it with
// sdcard_mount(): the card may be on its way out. Not during `sd check` either. True if a card
// is mounted.
bool sdcard_mount_retry(void);

// Is a card mounted now? Never mounts one.
bool sdcard_mounted(void);

// What `sd info` shows, as values (for Lua's sos.sd.info()). Mounts the card if it can; false
// (and *info zeroed) if there's no card or it won't mount. total and free are the volume's bytes;
// free_ok is false if counting the free space failed.
typedef struct {
    const char *card;           // "SDHC/SDXC", ...
    uint64_t card_bytes;
    bool encrypted;
    const char *fs;             // "exFAT", "FAT32", ...
    uint64_t total, free;
    uint32_t cluster;
    bool free_ok;
} sdcard_info_t;
bool sdcard_info(sdcard_info_t *info);
// Changes every time a card is mounted (the same card again too).
uint32_t sdcard_generation(void);

// What the `card` health check needs to know before it touches the card (it never mounts one):
// whether a card is mounted and what it is, and whether anything else owns the card now. Reads
// state only; touches neither the card nor the driver.
typedef struct {
    bool mounted;
    bool encrypted;             // mounted through the encryption layer
    bool locked;                // an encrypted card is in and this board's key doesn't open it
    const char *fs;             // "exFAT", "FAT32", ... (mounted)
    const char *busy;           // NULL, or why the card belongs to something else now:
                                // "sd check running", "card being formatted",
                                // "card unmounted by hand"
    uint32_t generation;        // sdcard_generation()
} sdcard_health_t;
void sdcard_health_state(sdcard_health_t *out);

// The `sd-check` health check (docs/design/health.md §9.7): `sd check`'s job without a shell, its
// report lines counted but not printed, its steps aimed at 6 ms. It starts only on a card that
// was unmounted by hand (`sd unmount`, or an earlier `sd check`) and never unmounts one; it ends
// as `sd check` ends, the card left unmounted and its key forgotten, the summary logged.
typedef enum {
    SDCARD_CHECK_STARTED,
    SDCARD_CHECK_MOUNTED,               // the card is mounted
    SDCARD_CHECK_NOT_BY_HAND,           // not mounted, but not unmounted by hand either
    SDCARD_CHECK_BUSY,                  // `sd check` or a format owns the card: why
    SDCARD_CHECK_NOT_OPENED,            // no card, refused, no memory: why
} sdcard_check_start_t;
sdcard_check_start_t sdcard_check_health_begin(char *why, size_t why_len);

// One step: FSCK_MORE, or FSCK_DONE / FSCK_FAILED with the checker's result copied to *res (and
// whether the card is encrypted); SDCARD_CHECK_ENDED after a cancel (or with no check running).
#define SDCARD_CHECK_ENDED (-1)
int sdcard_check_health_step(bool cancel, fsck_result_t *res, bool *encrypted);

// After the configuration was erased: an encrypted card is unmounted and its key forgotten (the
// owner secret it was opened with no longer exists). A running `sd check` on it stops.
void sdcard_owner_gone(void);

#endif
