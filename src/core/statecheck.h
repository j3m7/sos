// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The pure half of phase 3's health checks, the checks of stored state (core/health.h,
// docs/design/health.md §9.3): `config`, `rootsecret`, `otp`, `failcount`, `hostkey` and `card`.
// statecheck_board.c reads flash, OTP and the card and hands what it found here; these judge it.
// tests/test_statecheck.c runs them on the host with made-up inputs, one test per status path.
//
// None of these checks holds a gate, and none repairs anything: a FAIL says what the board is
// doing about it (in phase 3, nothing new), never that something is refused.

#ifndef SOS_STATECHECK_H
#define SOS_STATECHECK_H

#include <stdbool.h>
#include <stdint.h>

#include "core/bootkey.h"
#include "core/config.h"
#include "core/failcount.h"
#include "core/health.h"
#include "core/roototp.h"

// --- config: both configuration records ---------------------------------------------------------

typedef struct {
    bool persistent;                    // settings_persistent(): the slots are past the image
    config_slot_info_t slot[2];         // config_slot_info() through settings.c's store
} statecheck_config_t;

// The record in use is config_in_use()'s, what config_load() took at boot. Slots are named A and
// B (0 and 1). PASS: in use sealed, the other empty or an older sealed record. WARN: in use plain
// (not sealed yet; or written after a sealed one: a downgrade, provisioning or an interrupted
// seal), the other slot damaged, sealed under another key, or a refused plain record left; no
// record in use but a refused plain one. FAIL: no usable record but a damaged or unopened one, or
// the slots unusable. SKIP: both empty.
void statecheck_judge_config(const statecheck_config_t *c, health_result_t *out);

// --- rootsecret: the root secret's source against the OTP flags --------------------------------

typedef struct {
    rootsrc_t source;                   // rootsource()
    roototp_find_t found;               // what rootsource_init() found in OTP
    unsigned page;                      // the OTP page it concerns (ROOTOTP_FIRST_PAGE + index)
    bool secret_ok;                     // FOUND: the secret passed its check
    bool locks_ok;                      // FOUND: the list's pages are locked against the bootloader
    unsigned open_page;                 // !locks_ok: the first that isn't
    bool soft_locked;                   // OTP source: its page's soft lock reads as set
    bool console_only;                  // consoleonly()
    bool flash_usable;                  // FLASH source: rootflash_usable()
    int flash_state;                    // FLASH source: rootflash_state() (ROOTSECRET_*)
    bool flash_degraded;                // FLASH source: rootflash_degraded()
} statecheck_root_t;

void statecheck_judge_root(const statecheck_root_t *r, health_result_t *out);

// --- otp: the OTP rows SOS relies on, and the boot keys ----------------------------------------
//
// Only these rows, all below 0x0c0: never the root secret's (pages 4 to 6, rows 0x100-0x1bf).
#define STATECHECK_CRIT1_ROW     0x040u         // eight copies, raw
#define STATECHECK_CRIT1_COPIES  8
#define STATECHECK_FLAGS1_ROW    0x04bu         // BOOT_FLAGS1, three copies, raw
#define STATECHECK_FLAGS1_COPIES 3
#define STATECHECK_BOOTKEY_ROW   0x080u         // BOOTKEY0_0 .. BOOTKEY3_15, error-corrected
#define STATECHECK_BOOTKEY_ROWS  (BOOTKEY_SLOTS * BOOTKEY_SLOT_ROWS)
// The flags SOS relies on in CRIT1 and the CRITICAL register (same positions): secure boot,
// secure debug off, debug off, boot architecture, glitch detector and its sensitivity. And in
// BOOT_FLAGS1: KEY_VALID and KEY_INVALID.
#define STATECHECK_CRIT1_FLAGS   0x7fu
#define STATECHECK_SECURE_BOOT   0x01u
#define STATECHECK_SECURE_DEBUG_DISABLE 0x02u
#define STATECHECK_DEBUG_DISABLE 0x04u
#define STATECHECK_KEY_VALID     0x00fu
#define STATECHECK_KEY_INVALID   0xf00u
#define STATECHECK_KEY_INVALID_LSB 8
#define STATECHECK_UNREADABLE    0xffffffffu    // what the raw alias returns for a row it can't read

typedef struct {
    uint32_t latched;                   // the CRITICAL register
    uint32_t crit1[STATECHECK_CRIT1_COPIES];        // raw rows
    uint32_t flags1[STATECHECK_FLAGS1_COPIES];      // raw rows
    uint32_t key_raw[STATECHECK_BOOTKEY_ROWS];      // raw rows
    uint16_t key_ecc[STATECHECK_BOOTKEY_ROWS];      // the same rows through the corrected alias
    bool image_signed;                  // the running image's key was found
    uint8_t image_hash[BOOTKEY_HASH_LEN];           // its SHA-256, as the boot ROM hashes it
    bool release;                       // a release build (NDEBUG: the release and size presets)
} statecheck_otp_t;

// The OTP macro's 6 parity bits over 16 data bits (a Hamming code with an overall parity bit),
// above the data: the 22 bits a correctly written row holds. As picotool's otp_calculate_ecc().
uint32_t statecheck_otp_ecc(uint16_t data);

typedef enum {
    STATECHECK_ROW_OK,                  // parity right, and the corrected alias agrees
    STATECHECK_ROW_UNREADABLE,          // the raw alias said all ones (locked, or damaged)
    STATECHECK_ROW_ECC_MISMATCH,        // the stored parity isn't that of the stored data
    STATECHECK_ROW_CORRECTED,           // the corrected alias returns other data than stored
} statecheck_row_t;

// One error-corrected row from its raw 24 bits (both bit-repair bits set: all 24 inverted first)
// and what the corrected alias returned for it.
statecheck_row_t statecheck_otp_row(uint32_t raw, uint16_t corrected);

// The repository's public development keys (keys/dev-INSECURE-<n>.pem): their OTP hashes, and
// which one a hash is (1 or 2), 0 for neither.
#define STATECHECK_DEV_KEYS 2
extern const uint8_t statecheck_dev_key_hash[STATECHECK_DEV_KEYS][BOOTKEY_HASH_LEN];
unsigned statecheck_dev_key(const uint8_t hash[BOOTKEY_HASH_LEN]);

// SKIP: secure boot off, no slot written, none marked valid. WARN, the first found: a row
// unreadable, a boot key row's ECC wrong or corrected, CRIT1's copies disagreeing or their vote
// differing from the latched register, BOOT_FLAGS1's copies disagreeing, a usable slot holding a
// dev key in a release build. FAIL: secure boot on and the image's key in no usable slot.
void statecheck_judge_otp(const statecheck_otp_t *o, health_result_t *out);

// --- failcount: the wrong-password sector ---------------------------------------------------------

typedef struct {
    bool usable;                        // failflash_usable()
    failcount_info_t info;              // failcount_inspect() of the sector
    unsigned count;                     // failflash_count(): what the console uses, RAM included
} statecheck_failcount_t;

void statecheck_judge_failcount(const statecheck_failcount_t *f, health_result_t *out);

// --- hostkey: the stored host key against the one sshd serves ---------------------------------

typedef struct {
    bool have_owner;                    // settings_has_owner_secret()
    bool have_key;                      // a host key is in use
    bool gave_up;                       // none, for a reason that lasts until a reboot
    bool unsealed;                      // in use as a plain record (no root secret, open board)
    bool page_matches;                  // the stored page's digest is the one taken when loaded
    const char *fingerprint;            // sshd's, "SHA256:..." (NULL: sshd not started)
    bool erased;                        // hostkey_erase() since: gone on purpose, not changed
} statecheck_hostkey_t;

void statecheck_judge_hostkey(const statecheck_hostkey_t *h, health_result_t *out);

// --- card: the protected directories and the approved roots ------------------------------------

typedef enum {
    STATECHECK_PATH_MISSING,
    STATECHECK_PATH_DIR,
    STATECHECK_PATH_FILE,
    STATECHECK_PATH_ERROR,              // f_stat failed otherwise
} statecheck_path_t;

typedef enum {
    STATECHECK_ROOTS_MATCH,
    STATECHECK_ROOTS_CHANGED,
    STATECHECK_ROOTS_MISSING,
    STATECHECK_ROOTS_TOO_BIG,
    STATECHECK_ROOTS_UNREADABLE,
} statecheck_roots_t;

typedef struct {
    const char *yield;                  // non-NULL: the card's owner is busy (the SKIP text)
    bool mounted;
    bool locked;            // an encrypted card is in and doesn't open
    const char *fs;                     // "exFAT", "FAT32"
    bool encrypted;
    statecheck_path_t var_log, certs;
    bool approved;                      // settings()->certs_hash is set
    statecheck_roots_t roots;           // approved: what /certs/roots.pem is
} statecheck_card_t;

void statecheck_judge_card(const statecheck_card_t *c, health_result_t *out);

// --- the board (statecheck_board.c): the checks' steps, for health_board.c's table ------------

health_step_t statecheck_config_step(void **state, bool cancel, health_result_t *out);
health_step_t statecheck_root_step(void **state, bool cancel, health_result_t *out);
health_step_t statecheck_otp_step(void **state, bool cancel, health_result_t *out);
health_step_t statecheck_failcount_step(void **state, bool cancel, health_result_t *out);
health_step_t statecheck_hostkey_step(void **state, bool cancel, health_result_t *out);
health_step_t statecheck_card_step(void **state, bool cancel, health_result_t *out);

#ifdef SOS_TEST_COMMANDS
// `health state-dump` (test builds; reads only, allowed over SSH): what each check reads, a line
// at a time, with the time each read took, so one board run shows why each check said what it
// said. Holds the main loop while it runs (a few milliseconds; the card isn't touched).
void statecheck_board_dump(health_print_fn print, void *arg);
#endif

#endif
