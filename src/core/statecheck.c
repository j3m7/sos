// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// The checks of stored state, the pure half (core/statecheck.h): pure C, host tested
// (tests/test_statecheck.c).

#include "core/statecheck.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/rootsecret.h"

// The result's status and text. Its line is cut to fit, as any text is.
__attribute__((format(printf, 3, 4)))
static void result(health_result_t *out, health_status_t s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out->text, sizeof(out->text), fmt, ap);
    va_end(ap);
    out->status = s;
}

// --- config ----------------------------------------------------------------------------------

static bool damaged(config_slot_kind_t k) {
    return k == CONFIG_SLOT_JUNK || k == CONFIG_SLOT_BAD_HEADER || k == CONFIG_SLOT_BAD_CRC ||
           k == CONFIG_SLOT_INVALID;
}

static char slot_name(unsigned slot) {
    return slot ? 'B' : 'A';
}

void statecheck_judge_config(const statecheck_config_t *c, health_result_t *out) {
    if (!c->persistent) {
        result(out, HEALTH_FAIL, "firmware overlaps the config slots; settings aren't saved");
        return;
    }
    const config_slot_info_t *s = c->slot;
    int use = config_in_use(s);
    if (use < 0) {
        for (unsigned i = 0; i < 2; i++)
            if (s[i].kind == CONFIG_SLOT_UNOPENED) {
                result(out, HEALTH_FAIL, "sealed record in %c doesn't open; running unconfigured",
                       slot_name(i));
                return;
            }
        for (unsigned i = 0; i < 2; i++)
            if (damaged(s[i].kind)) {
                result(out, HEALTH_FAIL, "slot %c %s, no usable record; running on defaults",
                       slot_name(i), config_slot_kind_name(s[i].kind));
                return;
            }
        for (unsigned i = 0; i < 2; i++)
            if (s[i].kind == CONFIG_SLOT_REFUSED) {
                result(out, HEALTH_WARN, "plain record refused (console-only); running unconfigured");
                return;
            }
        result(out, HEALTH_SKIP, "nothing saved yet (run setup)");
        return;
    }
    unsigned u = (unsigned)use, o = 1 - u;
    unsigned seq = (unsigned)s[u].seq;
    config_slot_kind_t other = s[o].kind;
    if (s[u].kind == CONFIG_SLOT_PLAIN) {
        // scan() takes a plain record before a sealed one whatever the numbers: one beside a
        // sealed record was written after it (older firmware after a downgrade, provisioning, or
        // a seal cut short before the plain copy was erased). Otherwise settings_init() should
        // have sealed it at boot.
        if (other == CONFIG_SLOT_SEALED)
            result(out, HEALTH_WARN, "plain rev %u (%c) in use, written after sealing "
                   "(downgrade?)", seq, slot_name(u));
        else
            result(out, HEALTH_WARN, "plain rev %u (%c) in use: not sealed yet (see dmesg)", seq,
                   slot_name(u));
        return;
    }
    // In use: sealed.
    if (damaged(other)) {
        result(out, HEALTH_WARN, "rev %u sealed, in use (%c); %c %s: no fallback", seq,
               slot_name(u), slot_name(o), config_slot_kind_name(other));
    } else if (other == CONFIG_SLOT_UNOPENED) {
        result(out, HEALTH_WARN, "rev %u sealed (%c); %c doesn't open (old root secret?)", seq,
               slot_name(u), slot_name(o));
    } else if (other == CONFIG_SLOT_REFUSED) {
        result(out, HEALTH_WARN, "rev %u sealed (%c); %c: refused plain left (console-only)", seq,
               slot_name(u), slot_name(o));
    } else if (other == CONFIG_SLOT_SEALED) {
        result(out, HEALTH_PASS, "rev %u sealed, in use (%c); %c rev %u sealed", seq,
               slot_name(u), slot_name(o), (unsigned)s[o].seq);
    } else {
        result(out, HEALTH_PASS, "rev %u sealed, in use (%c); %c empty", seq, slot_name(u),
               slot_name(o));
    }
}

// --- rootsecret ------------------------------------------------------------------------------

void statecheck_judge_root(const statecheck_root_t *r, health_result_t *out) {
    switch (r->source) {
    case ROOTSRC_OTP:
        if (!r->soft_locked)
            result(out, HEALTH_WARN, "OTP page %u not locked for this boot (readable by firmware)",
                   r->page);
        else if (!r->locks_ok)
            result(out, HEALTH_WARN, "OTP page %u; page %u writable from the bootloader", r->page,
                   r->open_page);
        else
            result(out, HEALTH_PASS, "OTP page %u, locked for this boot; list pages locked",
                   r->page);
        return;
    case ROOTSRC_FLASH:
        // roototp_choose() never gives a console-only board the flash: checked as an invariant.
        if (r->console_only)
            result(out, HEALTH_FAIL, "console-only board using a flash secret (a bug); keys in use");
        else if (!r->flash_usable)
            result(out, HEALTH_FAIL, "the firmware overlaps the flash record; there are no keys");
        else if (r->flash_state == ROOTSECRET_NONE)
            result(out, HEALTH_FAIL, "none in flash (making it failed, see dmesg); no keys");
        else if (r->flash_state != ROOTSECRET_OK)
            result(out, HEALTH_FAIL, "flash record unusable (damaged or newer); there are no keys");
        else if (r->flash_degraded)
            result(out, HEALTH_WARN, "in flash, one copy damaged; the other in use");
        else if (r->found == ROOTOTP_EXHAUSTED)
            result(out, HEALTH_WARN, "in flash; every OTP page for it is spent");
        else
            result(out, HEALTH_PASS, "in flash, both copies intact (open board)");
        return;
    case ROOTSRC_NONE:
        break;
    }
    // The reasons rootsource_init() gives, shortened to a line.
    if (r->found == ROOTOTP_FOUND && r->secret_ok)
        result(out, HEALTH_FAIL, "none: page %u writable from the bootloader; there are no keys",
               r->open_page);
    else if (r->found == ROOTOTP_FOUND)
        result(out, HEALTH_FAIL, "none: OTP page %u's secret fails its check; no keys", r->page);
    else if (r->found == ROOTOTP_BAD_MARKER)
        result(out, HEALTH_FAIL, "none: OTP page %u's marker is damaged; there are no keys",
               r->page);
    else if (r->found == ROOTOTP_CANT_READ)
        result(out, HEALTH_FAIL, "none: OTP page %u can't be read; there are no keys", r->page);
    else if (r->found == ROOTOTP_EXHAUSTED)
        result(out, HEALTH_FAIL, "none: OTP list spent (console-only board); no keys");
    else
        result(out, HEALTH_FAIL, "none: nothing in OTP (console-only board); no keys");
}

// --- otp -------------------------------------------------------------------------------------

static unsigned parity(uint32_t v) {
    return (unsigned)__builtin_popcount(v) & 1u;
}

// The OTP macro's parity bits for 16 data bits, as picotool's otp_calculate_ecc() (its source:
// the macro's data book, "parity bit generation map for 16 bit user data"): five Hamming bits
// and an overall parity bit, above the data.
uint32_t statecheck_otp_ecc(uint16_t x) {
    uint32_t p0 = parity(x & 0xad5bu);      // 0b1010110101011011
    uint32_t p1 = parity(x & 0x366du);      // 0b0011011001101101
    uint32_t p2 = parity(x & 0xc78eu);      // 0b1100011110001110
    uint32_t p3 = parity(x & 0x07f0u);      // 0b0000011111110000
    uint32_t p4 = parity(x & 0xf800u);      // 0b1111100000000000
    uint32_t p5 = parity(x) ^ p0 ^ p1 ^ p2 ^ p3 ^ p4;
    uint32_t p = p0 | p1 << 1 | p2 << 2 | p3 << 3 | p4 << 4 | p5 << 5;
    return (uint32_t)x | p << 16;
}

statecheck_row_t statecheck_otp_row(uint32_t raw, uint16_t corrected) {
    if (raw == STATECHECK_UNREADABLE)
        return STATECHECK_ROW_UNREADABLE;
    raw &= 0xffffffu;
    if ((raw >> 22) == 3u)                  // both bit-repair bits: the row is stored inverted
        raw ^= 0xffffffu;
    uint16_t data = (uint16_t)raw;
    if ((raw & 0x3fffffu) != statecheck_otp_ecc(data))
        return STATECHECK_ROW_ECC_MISMATCH;
    if (corrected != data)
        return STATECHECK_ROW_CORRECTED;
    return STATECHECK_ROW_OK;
}

// SHA-256 of the 64-byte public key (x then y) of each, as the boot ROM hashes a boot key and as
// keys/README.md lists them; computed with
//   openssl ec -in keys/dev-INSECURE-<n>.pem -pubout -outform DER | tail -c 64 | sha256sum
// tests/test_statecheck.c computes them again from the .pem files when openssl is installed.
const uint8_t statecheck_dev_key_hash[STATECHECK_DEV_KEYS][BOOTKEY_HASH_LEN] = {
    {0xe2, 0xf7, 0xcf, 0xf0, 0x33, 0x60, 0x0f, 0x5a, 0xb0, 0x0e, 0x33, 0xb6, 0xf8, 0x91, 0xa0, 0xff,
     0x6e, 0x63, 0x5a, 0xd0, 0xf2, 0x08, 0x86, 0x13, 0xca, 0x8b, 0x71, 0xb5, 0xf5, 0x6d, 0x0d, 0x51},
    {0xb9, 0x7c, 0x0f, 0xd7, 0xef, 0xb6, 0x89, 0x37, 0xe7, 0x33, 0xc4, 0x91, 0x6b, 0x44, 0xce, 0x4f,
     0x94, 0x01, 0x41, 0xc4, 0xe6, 0xec, 0xc9, 0x09, 0x94, 0xc9, 0x41, 0xf3, 0x96, 0xd7, 0xc9, 0x10},
};

unsigned statecheck_dev_key(const uint8_t hash[BOOTKEY_HASH_LEN]) {
    for (unsigned i = 0; i < STATECHECK_DEV_KEYS; i++)
        if (!memcmp(hash, statecheck_dev_key_hash[i], BOOTKEY_HASH_LEN))
            return i + 1;
    return 0;
}

// Bits (of mask) that some copies have and others don't.
static uint32_t disagree(const uint32_t *rows, unsigned n, uint32_t mask) {
    uint32_t any = 0, all = mask;
    for (unsigned i = 0; i < n; i++) {
        any |= rows[i] & mask;
        all &= rows[i];
    }
    return any & ~all;
}

void statecheck_judge_otp(const statecheck_otp_t *o, health_result_t *out) {
    // Rows that can't be read: locked or damaged, which the check can't tell apart.
    for (unsigned i = 0; i < STATECHECK_CRIT1_COPIES; i++)
        if (o->crit1[i] == STATECHECK_UNREADABLE) {
            result(out, HEALTH_WARN, "row 0x%03x unreadable (locked or damaged)",
                   STATECHECK_CRIT1_ROW + i);
            return;
        }
    for (unsigned i = 0; i < STATECHECK_FLAGS1_COPIES; i++)
        if (o->flags1[i] == STATECHECK_UNREADABLE) {
            result(out, HEALTH_WARN, "row 0x%03x unreadable (locked or damaged)",
                   STATECHECK_FLAGS1_ROW + i);
            return;
        }
    for (unsigned i = 0; i < STATECHECK_BOOTKEY_ROWS; i++)
        if (o->key_raw[i] == STATECHECK_UNREADABLE) {
            result(out, HEALTH_WARN, "row 0x%03x unreadable (locked or damaged)",
                   STATECHECK_BOOTKEY_ROW + i);
            return;
        }

    bootkey_otp_t keys;
    uint32_t flags = bootkey_vote(o->flags1, STATECHECK_FLAGS1_COPIES, 2);
    keys.valid = (uint8_t)(flags & STATECHECK_KEY_VALID);
    keys.invalid = (uint8_t)((flags & STATECHECK_KEY_INVALID) >> STATECHECK_KEY_INVALID_LSB);
    for (unsigned s = 0; s < BOOTKEY_SLOTS; s++)
        bootkey_slot_hash(o->key_ecc + s * BOOTKEY_SLOT_ROWS, keys.slot[s]);
    bool secure_boot = o->latched & STATECHECK_SECURE_BOOT;
    bool any_key = keys.valid != 0;
    for (unsigned s = 0; s < BOOTKEY_SLOTS; s++)
        any_key = any_key || !bootkey_slot_blank(&keys, s);
    if (!secure_boot && !any_key) {
        result(out, HEALTH_SKIP, "open board: secure boot off, no boot keys");
        return;
    }

    for (unsigned i = 0; i < STATECHECK_BOOTKEY_ROWS; i++) {
        statecheck_row_t r = statecheck_otp_row(o->key_raw[i], o->key_ecc[i]);
        if (r == STATECHECK_ROW_ECC_MISMATCH) {
            result(out, HEALTH_WARN, "boot key slot %u row 0x%03x: ECC mismatch (raw 0x%06x)",
                   i / BOOTKEY_SLOT_ROWS, STATECHECK_BOOTKEY_ROW + i,
                   (unsigned)(o->key_raw[i] & 0xffffffu));
            return;
        }
        if (r == STATECHECK_ROW_CORRECTED) {
            result(out, HEALTH_WARN, "boot key slot %u row 0x%03x: corrected (raw 0x%06x)",
                   i / BOOTKEY_SLOT_ROWS, STATECHECK_BOOTKEY_ROW + i,
                   (unsigned)(o->key_raw[i] & 0xffffffu));
            return;
        }
    }

    uint32_t crit_split = disagree(o->crit1, STATECHECK_CRIT1_COPIES, STATECHECK_CRIT1_FLAGS);
    uint32_t crit = bootkey_vote(o->crit1, STATECHECK_CRIT1_COPIES, 3) & STATECHECK_CRIT1_FLAGS;
    uint32_t latched = o->latched & STATECHECK_CRIT1_FLAGS;
    if (crit != latched) {
        result(out, HEALTH_WARN, "CRIT1 rows 0x%02x vs latched 0x%02x: written since reset/"
               "damaged", (unsigned)crit, (unsigned)latched);
        return;
    }
    if (crit_split) {
        result(out, HEALTH_WARN, "CRIT1 copies disagree on bits 0x%02x", (unsigned)crit_split);
        return;
    }
    uint32_t flags_split = disagree(o->flags1, STATECHECK_FLAGS1_COPIES,
                                    STATECHECK_KEY_VALID | STATECHECK_KEY_INVALID);
    if (flags_split) {
        result(out, HEALTH_WARN, "BOOT_FLAGS1 copies disagree on bits 0x%03x",
               (unsigned)flags_split);
        return;
    }

    int image_slot = -1;
    if (o->image_signed && bootkey_compare(&keys, o->image_hash, &image_slot) != BOOTKEY_MATCH)
        image_slot = -1;
    if (secure_boot && image_slot < 0) {
        result(out, HEALTH_FAIL, "image key in no valid slot: no boot after a reset; "
               "running on");
        return;
    }

    // "valid 0=dev1 1=dev2 2=key"
    char slots[48] = "";
    size_t len = 0;
    for (unsigned s = 0; s < BOOTKEY_SLOTS; s++) {
        if (!bootkey_slot_usable(&keys, s))
            continue;
        unsigned dev = statecheck_dev_key(keys.slot[s]);
        if (dev && o->release) {
            result(out, HEALTH_WARN, "valid slot %u holds public dev key %u (release build)", s,
                   dev);
            return;
        }
        int n = dev ? snprintf(slots + len, sizeof(slots) - len, " %u=dev%u", s, dev)
                    : snprintf(slots + len, sizeof(slots) - len, " %u=key", s);
        if (n > 0 && (size_t)n < sizeof(slots) - len)
            len += (size_t)n;
    }
    const char *debug = o->latched & STATECHECK_DEBUG_DISABLE ? "debug off"
                      : o->latched & STATECHECK_SECURE_DEBUG_DISABLE ? "secure debug off"
                      : "debug on";
    char image[24];
    if (image_slot >= 0)
        snprintf(image, sizeof(image), "image key in %d", image_slot);
    else
        snprintf(image, sizeof(image), "image key %s", o->image_signed ? "in none" : "unknown");
    result(out, HEALTH_PASS, "secure boot %s, %s; valid%s; %s", secure_boot ? "on" : "off", debug,
           len ? slots : " none", image);
}

// --- failcount -------------------------------------------------------------------------------

void statecheck_judge_failcount(const statecheck_failcount_t *f, health_result_t *out) {
    if (!f->usable) {
        result(out, HEALTH_FAIL, "firmware overlaps the sector; wrong passwords in RAM only");
        return;
    }
    const failcount_info_t *i = &f->info;
    unsigned cleared = i->cleared > i->attempts ? i->attempts : i->cleared;
    unsigned in_flash = i->state == FAILCOUNT_SECTOR_MARKED ? i->attempts - cleared : 0;
    if (f->count > in_flash) {
        result(out, HEALTH_WARN, "%u counted in RAM only: flash writes failed",
               f->count - in_flash);
    } else if (i->state == FAILCOUNT_SECTOR_OTHER) {
        result(out, HEALTH_WARN, "no marker: counts as 0 until the next password check");
    } else if (i->state == FAILCOUNT_SECTOR_MARKED && (i->stray || i->cleared_over)) {
        result(out, HEALTH_WARN, "tracks inconsistent (attempts %u, cleared %u): read as %u",
               i->attempts, i->cleared, in_flash);
    } else if (f->count) {
        result(out, HEALTH_WARN, "%u wrong password%s since the last right one", f->count,
               f->count == 1 ? "" : "s");
    } else if (i->state == FAILCOUNT_SECTOR_MARKED) {
        result(out, HEALTH_PASS, "0 wrong passwords; %u of %u checks used", i->attempts,
               (unsigned)FAILCOUNT_CAPACITY);
    } else {
        result(out, HEALTH_PASS, "no wrong passwords recorded");
    }
}

// --- hostkey ---------------------------------------------------------------------------------

void statecheck_judge_hostkey(const statecheck_hostkey_t *h, health_result_t *out) {
    if (!h->have_key) {
        if (!h->have_owner)
            result(out, HEALTH_SKIP, "board not set up: no host key yet");
        else if (h->gave_up)
            result(out, HEALTH_FAIL, "no host key (see dmesg); sshd isn't running");
        else
            result(out, HEALTH_SKIP, "no host key asked for yet");
        return;
    }
    const char *fp = h->fingerprint ? h->fingerprint : "sshd not started";
    if (!h->page_matches)
        result(out, HEALTH_FAIL, "stored record changed since loaded; sshd serves the key in RAM");
    else if (h->unsealed)
        result(out, HEALTH_WARN, "stored unsealed (no root secret); %s", fp);
    else
        result(out, HEALTH_PASS, "sealed, as loaded; %s", fp);
}

// --- card ------------------------------------------------------------------------------------

void statecheck_judge_card(const statecheck_card_t *c, health_result_t *out) {
    if (c->yield) {
        result(out, HEALTH_SKIP, "%s", c->yield);
        return;
    }
    if (!c->mounted) {
        if (c->locked)
            result(out, HEALTH_WARN, "encrypted card doesn't open with this board's key; not "
                   "mounted");
        else
            result(out, HEALTH_SKIP, "no card mounted");
        return;
    }
    if (c->var_log == STATECHECK_PATH_MISSING) {
        result(out, HEALTH_WARN, "/var/log missing: the log files aren't being written");
        return;
    }
    if (c->var_log == STATECHECK_PATH_FILE) {
        result(out, HEALTH_WARN, "/var/log is a file: no log files; rm it on the console");
        return;
    }
    if (c->var_log == STATECHECK_PATH_ERROR || c->certs == STATECHECK_PATH_ERROR) {
        result(out, HEALTH_WARN, "reading the card's directories failed");
        return;
    }
    if (c->certs == STATECHECK_PATH_FILE) {
        result(out, HEALTH_WARN, "/certs is a file: remove it with rm on the console");
        return;
    }
    char head[32];
    snprintf(head, sizeof(head), "%s %s", c->fs ? c->fs : "?", c->encrypted ? "encrypted" : "plain");
    if (!c->approved) {
        if (c->certs == STATECHECK_PATH_DIR)
            result(out, HEALTH_PASS, "%s; /var/log ok; unapproved roots: built-in only", head);
        else
            result(out, HEALTH_PASS, "%s; /var/log ok; no /certs: built-in roots only", head);
        return;
    }
    switch (c->roots) {
    case STATECHECK_ROOTS_MATCH:
        result(out, HEALTH_PASS, "%s; /var/log ok; roots.pem matches its approval", head);
        break;
    case STATECHECK_ROOTS_CHANGED:
        result(out, HEALTH_WARN, "roots.pem changed since approval: built-in roots only");
        break;
    case STATECHECK_ROOTS_MISSING:
        result(out, HEALTH_WARN, "approved roots.pem missing: built-in roots only");
        break;
    case STATECHECK_ROOTS_TOO_BIG:
        result(out, HEALTH_WARN, "roots.pem over 64 KB: built-in roots only");
        break;
    case STATECHECK_ROOTS_UNREADABLE:
        result(out, HEALTH_WARN, "roots.pem unreadable: built-in roots only");
        break;
    }
}
