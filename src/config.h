// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Persistent device configuration: Wi-Fi network, hostname, SSH user and trusted public keys.
//
// Stored as one record in two alternating flash slots (A/B). Each save goes to the slot not
// holding the current record, with a higher sequence number; loading picks the valid record with
// the highest sequence number, so a power cut during a save leaves the previous version in place.
// Storage is reached through config_store_t so the logic runs (and is tested) without flash.
//
// Pure C: no Pico SDK or wolfSSL dependency.

#ifndef SOS_CONFIG_H
#define SOS_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONFIG_SSID_MAX      32     // 802.11 SSID limit
#define CONFIG_PASSWORD_MAX  63     // WPA2 passphrase limit
#define CONFIG_HOSTNAME_MAX  32
#define CONFIG_USER_MAX      32
#define CONFIG_MAX_KEYS      8
#define CONFIG_KEY_BLOB_MAX  128    // SSH wire-format public key (Ed25519 51, ECDSA P-256 104)
#define CONFIG_KEY_COMMENT_MAX 40
#define CONFIG_NTP_SYNC_HOURS_MAX 720       // 30 days
#define CONFIG_NTP_SERVER_MAX 63

typedef struct {
    uint16_t len;                               // blob length; 0 = unused entry
    uint8_t blob[CONFIG_KEY_BLOB_MAX];          // as in authorized_keys, base64-decoded
    char comment[CONFIG_KEY_COMMENT_MAX + 1];
} config_key_t;

typedef struct {
    char wifi_ssid[CONFIG_SSID_MAX + 1];        // empty = not configured
    char wifi_password[CONFIG_PASSWORD_MAX + 1];// empty = open network
    char hostname[CONFIG_HOSTNAME_MAX + 1];
    char ssh_user[CONFIG_USER_MAX + 1];
    uint8_t key_count;
    config_key_t keys[CONFIG_MAX_KEYS];
    // Fields below were added later. The layout is append-only: a record saved by older
    // firmware is shorter, and the fields it lacks load as 0, so 0 must mean "default".
    uint16_t ntp_sync_hours;                    // periodic NTP sync interval; 0 = off
    char ntp_server[CONFIG_NTP_SERVER_MAX + 1]; // host name or IPv4 address; empty = build default
} sos_config_t;

// --- storage ------------------------------------------------------------------------------------

#define CONFIG_SLOT_SIZE 4096               // one flash sector per slot

typedef struct {
    // Contents of slot 0 or 1 (CONFIG_SLOT_SIZE bytes; erased flash reads as 0xff).
    const uint8_t *(*read)(void *ctx, unsigned slot);
    // Replace slot 0 or 1 with len bytes of data (len <= CONFIG_SLOT_SIZE). True on success.
    bool (*write)(void *ctx, unsigned slot, const void *data, size_t len);
    void *ctx;
} config_store_t;

// Defaults: hostname "sos", user "admin", no Wi-Fi, no keys.
void config_defaults(sos_config_t *cfg);

// Load the newest valid record. Returns false (and leaves *cfg as defaults) if neither slot
// holds one. *seq, if not NULL, receives the record's sequence number.
bool config_load(const config_store_t *store, sos_config_t *cfg, uint32_t *seq);

// Save cfg to the slot not holding the newest valid record. Returns false if the write failed
// or the written record doesn't read back as valid; the previous record is then still in place.
bool config_save(const config_store_t *store, const sos_config_t *cfg);

// Erase both records (factory reset): the next load returns defaults.
bool config_erase(const config_store_t *store);

// --- validation and editing -------------------------------------------------------------------

// Each returns NULL if valid, otherwise a short reason.
const char *config_check_ssid(const char *ssid);
const char *config_check_password(const char *password);
const char *config_check_hostname(const char *hostname);
const char *config_check_user(const char *user);
// A DNS name or IPv4 address: letters, digits, '-' and '.', no empty labels.
const char *config_check_ntp_server(const char *server);

// Parse an authorized_keys line: "<type> <base64> [comment]". Supported types: ssh-ed25519,
// ecdsa-sha2-nistp256. Returns NULL on success, otherwise a short reason.
const char *config_key_parse(const char *line, config_key_t *key);

// Key type name from the blob ("ssh-ed25519", ...), or "?" if malformed.
const char *config_key_type(const config_key_t *key, char *buf, size_t buflen);

// Add a key. Returns NULL on success, otherwise a reason (duplicate, list full).
const char *config_key_add(sos_config_t *cfg, const config_key_t *key);

// Remove key `index` (0-based), keeping the order of the rest. False if out of range.
bool config_key_remove(sos_config_t *cfg, unsigned index);

// True if blob matches one of the configured keys.
bool config_key_find(const sos_config_t *cfg, const uint8_t *blob, size_t len);

// --- helpers (exposed for tests) ----------------------------------------------------------------

uint32_t config_crc32(const void *data, size_t len);

// Decode base64 (standard alphabet, '=' padding). Returns the decoded length, or -1 if the
// input is malformed or doesn't fit in outlen.
int config_base64_decode(const char *in, size_t inlen, uint8_t *out, size_t outlen);

#endif
