// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// Persistent device configuration: Wi-Fi network and power saving, hostname, trusted SSH public
// keys, time sync, approved TLS roots.
//
// Stored as one record in two alternating flash slots (A/B), sealed on the device (see "storage"
// below). Each save goes to the slot not
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

#include "core/sealer.h"

#define CONFIG_SSID_MAX      32     // 802.11 SSID limit
#define CONFIG_PASSWORD_MAX  63     // WPA2 passphrase limit
#define CONFIG_HOSTNAME_MAX  32
#define CONFIG_USER_MAX      32
#define CONFIG_MAX_KEYS      8
#define CONFIG_KEY_BLOB_MAX  128    // SSH wire-format public key (Ed25519 51, ECDSA P-256 104)
#define CONFIG_KEY_COMMENT_MAX 40
#define CONFIG_NTP_SYNC_HOURS_MAX 720       // 30 days
#define CONFIG_LOG_ROTATE_DAYS_DEFAULT 7     // a week
#define CONFIG_LOG_ROTATE_DAYS_MAX 365
#define CONFIG_LOG_ROTATE_MINUTE_MAX 1439   // 23:59
#define CONFIG_NTP_SERVER_MAX 63
#define CONFIG_CERTS_HASH_LEN 32    // SHA-256
#define CONFIG_RTC_OFFSET_MIN (-64)
#define CONFIG_RTC_OFFSET_MAX 63
#define CONFIG_TEMP_OFFSET_MAX 150          // tenths of a degree: the chip temperature's
                                            // calibration is at most 15.0 degrees either way
#define CONFIG_OWNER_SECRET_LEN 32
#define CONFIG_CONSOLE_PW_MIN 8
#define CONFIG_CONSOLE_PW_MAX 64
#define CONFIG_CONSOLE_SALT_LEN 16
#define CONFIG_CONSOLE_VERIFIER_LEN 32
#define CONFIG_CONSOLE_ITERATIONS_MIN 1000      // as core/pwverify.h accepts
#define CONFIG_CONSOLE_ITERATIONS_MAX 1000000
// sos_config_t.glitch_detector as stored: 0 = not chosen, which means the default (armed at the
// highest sensitivity); 1..4 = armed at sensitivity 0..3; 5..8 = watching at sensitivity 0..3;
// 9 = off. config_glitch_mode() turns that into what core/glitchdet.h takes.
#define CONFIG_GLITCH_DETECTOR_DEFAULT 0
#define CONFIG_GLITCH_DETECTOR_OFF     9
#define CONFIG_GLITCH_DETECTOR_MAX     9
#define CONFIG_GLITCH_DEFAULT_MODE     4        // armed at sensitivity 3
// Saved Wi-Fi networks: the first in wifi_ssid/wifi_password, the others in wifi_extra[].
#define CONFIG_WIFI_EXTRA 4
#define CONFIG_WIFI_NETS  (1 + CONFIG_WIFI_EXTRA)

typedef struct {
    uint16_t len;                               // blob length; 0 = unused entry
    uint8_t blob[CONFIG_KEY_BLOB_MAX];          // as in authorized_keys, base64-decoded
    char comment[CONFIG_KEY_COMMENT_MAX + 1];
} config_key_t;

// A Wi-Fi network: SSID (empty = unused entry) and passphrase (empty = open network).
typedef struct {
    char ssid[CONFIG_SSID_MAX + 1];
    char password[CONFIG_PASSWORD_MAX + 1];
} config_wifi_net_t;

typedef struct {
    // The first saved Wi-Fi network (see wifi_extra for the others).
    char wifi_ssid[CONFIG_SSID_MAX + 1];        // empty = not configured
    char wifi_password[CONFIG_PASSWORD_MAX + 1];// empty = open network
    char hostname[CONFIG_HOSTNAME_MAX + 1];
    // No longer used: SSH accepts any user name (the trusted keys decide). Kept, still "admin"
    // by default, so the record's layout doesn't change and older firmware, which requires the
    // name to match, still has one to match.
    char ssh_user[CONFIG_USER_MAX + 1];
    uint8_t key_count;
    config_key_t keys[CONFIG_MAX_KEYS];
    // Fields below were added later. The layout is append-only: a record saved by older
    // firmware is shorter, and the fields it lacks load as 0, so 0 must mean "default".
    uint16_t ntp_sync_hours;                    // periodic NTS sync interval; 0 = off
    char ntp_server[CONFIG_NTP_SERVER_MAX + 1]; // NTS-KE server; empty = build default (SOS_NTS_SERVER)
    // SHA-256 of the approved /certs/roots.pem (extra TLS roots on the SD card); all zero = none.
    uint8_t certs_hash[CONFIG_CERTS_HASH_LEN];
    uint8_t wifi_powersave;                     // 1 = Wi-Fi power saving on; 0 = off (default)
    // RTC calibration: the PCF8523 offset register's value, -64..63 steps of 4.34 ppm; positive
    // slows the RTC. 0 = none. This byte was padding (zero) in records saved before it had a
    // use, so those load as "none", and the record's length doesn't change.
    int8_t rtc_offset;
    // Two capital letters (ISO 3166, e.g. "US"): the radio rules to follow. Empty = the chip's
    // worldwide set, which is allowed everywhere but may perform worse.
    char wifi_country[4];
    // The owner secret: random bytes that tie the SD cards this board encrypts to this
    // configuration (files/cardkey.h). All zero = none yet. It exists only in a sealed record:
    // a plain record never carries one (it's zeroed on load and on a plain save), so a
    // configuration that arrives plain gets a new owner. Erasing the configuration destroys it.
    // Never shown, logged or exported.
    uint8_t owner_secret[CONFIG_OWNER_SECRET_LEN];
    // The console password, as its verifier (core/pwverify.h): the password is asked for
    // before the console takes any command. console_iterations 0 = no password set. Like the
    // owner secret these three exist only in a sealed record (zeroed when a plain record
    // loads, never written plain): the verifier is keyed with a key from the board's root
    // secret, so nothing outside the board can make one.
    uint32_t console_iterations;
    uint8_t console_salt[CONFIG_CONSOLE_SALT_LEN];
    uint8_t console_verifier[CONFIG_CONSOLE_VERIFIER_LEN];
    // The other way round: only in a plain record. The provisioning tool puts the console
    // password to set here; the board turns it into the verifier when it seals the record,
    // and it is never written sealed (zeroed before sealing). Empty = none given.
    char console_password[CONFIG_CONSOLE_PW_MAX + 1];
    // The glitch detector, set up by software at boot (core/glitchdet.h): 0 = the default
    // (armed at the highest sensitivity), 1..4 = armed at sensitivity 0..3 (a trigger resets
    // the board), 5..8 = watching at sensitivity 0..3 (a trigger is only logged), 9 = off.
    // Not protection (see glitchdet.h). This byte was padding (zero) in records saved before it
    // had a use, so those load as the default and the record's length doesn't change.
    uint8_t glitch_detector;
    // The chip temperature's calibration (`temp cal`, core/sensors.h): tenths of a degree added
    // to every reading, -CONFIG_TEMP_OFFSET_MAX..CONFIG_TEMP_OFFSET_MAX; 0 = uncalibrated. These
    // two bytes were padding (zero) in every record saved before they had a use, so those load
    // as uncalibrated and the record's length doesn't change: firmware without the field still
    // reads a record that has it.
    int16_t temp_offset;
    // The other saved Wi-Fi networks, in the order they are preferred, after wifi_ssid. Used
    // entries come first; all are unused while wifi_ssid is empty. Edit them with the
    // config_wifi_*() functions, which keep that so.
    config_wifi_net_t wifi_extra[CONFIG_WIFI_EXTRA];
    // Log file rotation (core/logfile.h): every log_rotate_days days (0 = the default,
    // CONFIG_LOG_ROTATE_DAYS_DEFAULT), at log_rotate_minute minutes after midnight UTC (0 to
    // 1439; 0 = 00:00, the default).
    uint16_t log_rotate_days;
    uint16_t log_rotate_minute;
} sos_config_t;

// The stored setting as glitchdet_start() and glitchdet_apply() take it: 0 = off, 1..4 = armed
// at sensitivity 0..3, 5..8 = watching at sensitivity 0..3.
static inline unsigned config_glitch_mode(unsigned stored) {
    if (stored == CONFIG_GLITCH_DETECTOR_OFF)
        return 0;
    return stored >= 1 && stored <= 8 ? stored : CONFIG_GLITCH_DEFAULT_MODE;
}

// --- storage ------------------------------------------------------------------------------------

#define CONFIG_SLOT_SIZE 4096               // one flash sector per slot

typedef struct {
    // Contents of slot 0 or 1 (CONFIG_SLOT_SIZE bytes; erased flash reads as 0xff).
    const uint8_t *(*read)(void *ctx, unsigned slot);
    // Replace slot 0 or 1 with len bytes of data (len <= CONFIG_SLOT_SIZE). True on success.
    bool (*write)(void *ctx, unsigned slot, const void *data, size_t len);
    void *ctx;
    // How records are sealed (the device: AES-256-GCM under a key derived from the root secret,
    // core/seal.h). NULL: records are saved plain and sealed ones can't be read (the host
    // provisioning tool, which has no key).
    const sealer_t *sealer;
    // Don't load plain records. For a secured board, where anyone can write the flash over USB
    // and a plain record would be a way to inject a configuration. An open board loads them
    // (records from older firmware, provisioning images) and seals them: see config_save().
    bool refuse_plain;
} config_store_t;

// What the newest usable record is.
enum { CONFIG_STORED_NONE, CONFIG_STORED_PLAIN, CONFIG_STORED_SEALED };
int config_stored(const config_store_t *store);

// Defaults: hostname "sos", user "admin", no Wi-Fi, no keys.
void config_defaults(sos_config_t *cfg);

// Load the current record: the valid one with the highest sequence number, except that a
// plain record a store accepts comes before a sealed one whatever the numbers (it was written
// after the sealing: by older firmware after a downgrade, or by provisioning). Returns false (and leaves *cfg as defaults) if neither slot
// holds one. *seq, if not NULL, receives the record's sequence number.
bool config_load(const config_store_t *store, sos_config_t *cfg, uint32_t *seq);

// Save cfg to the slot not holding the newest valid record. Returns false if the write failed
// or the written record doesn't read back as valid; the previous record is then still in place.
// With a sealer the record is sealed, and once it's stored a plain record in the other slot is
// erased, so a configuration that arrived plain doesn't stay readable.
bool config_save(const config_store_t *store, const sos_config_t *cfg);

// Erase a slot that has a plain record's header but isn't a record this store loads (damaged,
// or any plain record in a store that refuses them), while the other slot holds the
// configuration sealed: it may still hold a password. A plain record the store does load is the
// current configuration and is left for config_save() to seal. Nothing to do without a
// sealer. False if a slot with a plain record's header is still there afterwards.
bool config_scrub_plain(const config_store_t *store);

// True if a slot holds a sealed record, intact as stored (its CRC is right), that this store
// can't open: sealed under another root secret, or changed since. Such a record counts as no
// record and is not erased automatically; this is for saying so.
bool config_sealed_unopened(const config_store_t *store);

// Erase both records (factory reset): the next load returns defaults.
bool config_erase(const config_store_t *store);

// --- validation and editing -------------------------------------------------------------------

// Each returns NULL if valid, otherwise a short reason.
const char *config_check_ssid(const char *ssid);
const char *config_check_password(const char *password);
const char *config_check_hostname(const char *hostname);
const char *config_check_user(const char *user);
// 8 to 64 printable ASCII characters (what the console's line editor takes).
const char *config_check_console_password(const char *password);
// Empty (worldwide) or two capital letters.
const char *config_check_country(const char *country);
// A DNS name or IPv4 address: letters, digits, '-' and '.', no empty labels.
const char *config_check_ntp_server(const char *server);

// --- Wi-Fi networks ------------------------------------------------------------------------------
// Numbered 0 (wifi_ssid) to config_wifi_count() - 1, in the order they are preferred.

unsigned config_wifi_count(const sos_config_t *cfg);

// Copy network `index` to *net. False if out of range.
bool config_wifi_get(const sos_config_t *cfg, unsigned index, config_wifi_net_t *net);

// The index of the network named ssid, or -1.
int config_wifi_find(const sos_config_t *cfg, const char *ssid);

// Add a network after the others. Returns NULL on success, otherwise a reason (invalid SSID or
// passphrase, already saved, list full).
const char *config_wifi_add(sos_config_t *cfg, const char *ssid, const char *password);

// Make ssid the first network, with this passphrase, in place of the current first one (or as
// the only one); a later entry with the same SSID is dropped. NULL on success, otherwise a reason.
const char *config_wifi_set_first(sos_config_t *cfg, const char *ssid, const char *password);

// Remove network `index`, keeping the order of the rest (removing 0 makes 1 the first). False
// if out of range.
bool config_wifi_remove(sos_config_t *cfg, unsigned index);

// Move network `index` to the front; the others keep their order. False if out of range.
bool config_wifi_prefer(sos_config_t *cfg, unsigned index);

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
