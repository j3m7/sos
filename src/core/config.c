// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "core/config.h"

#include <string.h>

#define CONFIG_MAGIC          0x43534f53u   // "SOSC"
#define CONFIG_FORMAT_VERSION 1

// Payload lengths written by each release, oldest first; a record must have one of them, so a
// field is never cut off partway. When adding fields to sos_config_t, append the new size.
#define CONFIG_V1_LENGTH offsetof(sos_config_t, ntp_sync_hours)    // up to the end of keys[]
_Static_assert(CONFIG_V1_LENGTH == 1540, "fields before ntp_sync_hours must not move");
#define CONFIG_V2_LENGTH offsetof(sos_config_t, certs_hash)        // + ntp_sync_hours, ntp_server
_Static_assert(CONFIG_V2_LENGTH == 1606, "fields before certs_hash must not move");
#define CONFIG_V3_LENGTH offsetof(sos_config_t, wifi_powersave)    // + certs_hash
_Static_assert(CONFIG_V3_LENGTH == 1638, "fields before wifi_powersave must not move");
#define CONFIG_V4_LENGTH offsetof(sos_config_t, wifi_country)      // + wifi_powersave, padding
_Static_assert(CONFIG_V4_LENGTH == 1640, "fields before wifi_country must not move");
static const uint16_t record_lengths[] = {
    CONFIG_V1_LENGTH,
    CONFIG_V2_LENGTH,
    CONFIG_V3_LENGTH,
    CONFIG_V4_LENGTH,
    sizeof(sos_config_t),   // + wifi_country
};

static bool known_length(uint16_t length) {
    for (size_t i = 0; i < sizeof(record_lengths) / sizeof(record_lengths[0]); i++)
        if (length == record_lengths[i])
            return true;
    return false;
}

// True if a record of `length` bytes contains all of `field`.
#define HAS_FIELD(length, field) \
    ((length) >= offsetof(sos_config_t, field) + sizeof(((sos_config_t *)0)->field))

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t length;        // payload bytes: one of record_lengths
    uint32_t seq;           // higher = newer (compared with wrap-around)
    uint32_t crc;           // CRC-32 over magic..seq and the payload
} record_header_t;

typedef struct {
    record_header_t hdr;
    sos_config_t cfg;
} record_t;

_Static_assert(sizeof(record_t) <= CONFIG_SLOT_SIZE, "config record must fit in one slot");

// --- CRC-32 (IEEE 802.3, as used by zlib) -------------------------------------------------------

static uint32_t crc32_update(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

uint32_t config_crc32(const void *data, size_t len) {
    return crc32_update(0, data, len);
}

// Covers the stored payload length; call only with a length already checked to be in range.
static uint32_t record_crc(const record_t *r) {
    uint32_t crc = crc32_update(0, &r->hdr, offsetof(record_header_t, crc));
    return crc32_update(crc, &r->cfg, r->hdr.length);
}

// --- records ------------------------------------------------------------------------------------

static bool terminated(const char *s, size_t size) {
    return memchr(s, '\0', size) != NULL;
}

// A record is valid if its header and CRC check out and its contents are within bounds.
static bool record_valid(const record_t *r) {
    if (r->hdr.magic != CONFIG_MAGIC || r->hdr.version != CONFIG_FORMAT_VERSION ||
        !known_length(r->hdr.length) || r->hdr.crc != record_crc(r))
        return false;
    const sos_config_t *c = &r->cfg;
    // Later fields: only check those the record contains (the rest load as 0).
    if (HAS_FIELD(r->hdr.length, ntp_sync_hours) && c->ntp_sync_hours > CONFIG_NTP_SYNC_HOURS_MAX)
        return false;
    if (HAS_FIELD(r->hdr.length, ntp_server) &&
        (!terminated(c->ntp_server, sizeof(c->ntp_server)) ||
         (c->ntp_server[0] && config_check_ntp_server(c->ntp_server))))
        return false;
    if (HAS_FIELD(r->hdr.length, wifi_powersave) && c->wifi_powersave > 1)
        return false;
    if (HAS_FIELD(r->hdr.length, wifi_country) &&
        (!terminated(c->wifi_country, sizeof(c->wifi_country)) ||
         config_check_country(c->wifi_country)))
        return false;
    if (!terminated(c->wifi_ssid, sizeof(c->wifi_ssid)) ||
        !terminated(c->wifi_password, sizeof(c->wifi_password)) ||
        !terminated(c->hostname, sizeof(c->hostname)) ||
        !terminated(c->ssh_user, sizeof(c->ssh_user)) || c->key_count > CONFIG_MAX_KEYS)
        return false;
    for (unsigned i = 0; i < c->key_count; i++)
        if (c->keys[i].len == 0 || c->keys[i].len > CONFIG_KEY_BLOB_MAX ||
            !terminated(c->keys[i].comment, sizeof(c->keys[i].comment)))
            return false;
    return true;
}

static bool seq_newer(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}

// Index of the slot holding the newest valid record, or -1.
static int newest_slot(const config_store_t *store, const record_t **out) {
    const record_t *r0 = (const record_t *)store->read(store->ctx, 0);
    const record_t *r1 = (const record_t *)store->read(store->ctx, 1);
    bool v0 = record_valid(r0), v1 = record_valid(r1);
    int slot = -1;
    if (v0 && v1)
        slot = seq_newer(r1->hdr.seq, r0->hdr.seq) ? 1 : 0;
    else if (v0)
        slot = 0;
    else if (v1)
        slot = 1;
    if (out)
        *out = slot == 0 ? r0 : slot == 1 ? r1 : NULL;
    return slot;
}

void config_defaults(sos_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->hostname, "sos");
    strcpy(cfg->ssh_user, "admin");
}

bool config_load(const config_store_t *store, sos_config_t *cfg, uint32_t *seq) {
    const record_t *r;
    config_defaults(cfg);
    if (newest_slot(store, &r) < 0)
        return false;
    memcpy(cfg, &r->cfg, r->hdr.length);       // fields a shorter record lacks stay 0
    if (seq)
        *seq = r->hdr.seq;
    return true;
}

bool config_save(const config_store_t *store, const sos_config_t *cfg) {
    const record_t *current;
    int slot = newest_slot(store, &current);
    static record_t rec;        // too big for the stack; saves are rare and single-threaded

    memset(&rec, 0, sizeof(rec));
    rec.hdr.magic = CONFIG_MAGIC;
    rec.hdr.version = CONFIG_FORMAT_VERSION;
    rec.hdr.length = sizeof(sos_config_t);
    rec.hdr.seq = slot < 0 ? 1 : current->hdr.seq + 1;
    memcpy(&rec.cfg, cfg, sizeof(rec.cfg));
    rec.hdr.crc = record_crc(&rec);

    unsigned target = slot == 0 ? 1 : 0;
    if (!store->write(store->ctx, target, &rec, sizeof(rec)))
        return false;
    const record_t *back = (const record_t *)store->read(store->ctx, target);
    return record_valid(back) && back->hdr.seq == rec.hdr.seq;
}

bool config_erase(const config_store_t *store) {
    static const uint8_t blank[sizeof(record_header_t)];   // zeroes: an invalid header
    return store->write(store->ctx, 0, blank, sizeof(blank)) &&
           store->write(store->ctx, 1, blank, sizeof(blank));
}

// --- validation ---------------------------------------------------------------------------------

const char *config_check_ssid(const char *ssid) {
    size_t n = strlen(ssid);
    if (n == 0)
        return "SSID is empty";
    if (n > CONFIG_SSID_MAX)
        return "SSID is longer than 32 bytes";
    return NULL;
}

const char *config_check_password(const char *password) {
    size_t n = strlen(password);
    if (n == 0)
        return NULL;                                    // open network
    if (n < 8 || n > CONFIG_PASSWORD_MAX)
        return "WPA2 passphrase must be 8 to 63 characters";
    for (const char *p = password; *p; p++)
        if (*p < 0x20 || *p > 0x7e)
            return "passphrase must be printable ASCII";
    return NULL;
}

const char *config_check_hostname(const char *hostname) {
    size_t n = strlen(hostname);
    if (n == 0 || n > CONFIG_HOSTNAME_MAX)
        return "hostname must be 1 to 32 characters";
    if (hostname[0] == '-' || hostname[n - 1] == '-')
        return "hostname can't start or end with '-'";
    for (const char *p = hostname; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'))
            return "hostname may only contain letters, digits and '-'";
    }
    return NULL;
}

const char *config_check_country(const char *country) {
    if (!country[0])
        return NULL;
    if (strlen(country) != 2 || country[0] < 'A' || country[0] > 'Z' || country[1] < 'A' ||
        country[1] > 'Z')
        return "country must be two capital letters (e.g. US), or empty for worldwide";
    return NULL;
}

const char *config_check_ntp_server(const char *server) {
    size_t n = strlen(server);
    if (n == 0 || n > CONFIG_NTP_SERVER_MAX)
        return "server must be 1 to 63 characters";
    if (server[0] == '.' || server[n - 1] == '.' || strstr(server, ".."))
        return "server name has an empty part ('..' or a '.' at either end)";
    for (const char *p = server; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '.'))
            return "server may only contain letters, digits, '-' and '.'";
    }
    return NULL;
}

const char *config_check_user(const char *user) {
    size_t n = strlen(user);
    if (n == 0 || n > CONFIG_USER_MAX)
        return "user name must be 1 to 32 characters";
    for (const char *p = user; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-'))
            return "user name may only contain letters, digits, '.', '_' and '-'";
    }
    return NULL;
}

// --- base64 -------------------------------------------------------------------------------------

static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int config_base64_decode(const char *in, size_t inlen, uint8_t *out, size_t outlen) {
    if (inlen % 4 != 0)
        return -1;
    size_t n = 0;
    for (size_t i = 0; i < inlen; i += 4) {
        int v[4], pad = 0;
        for (int k = 0; k < 4; k++) {
            char c = in[i + k];
            if (c == '=' && i + 4 == inlen && k >= 2) {
                v[k] = 0;
                pad++;
            } else if (pad || (v[k] = b64_value(c)) < 0) {
                return -1;                              // bad character, or data after '='
            }
        }
        uint32_t bits = (uint32_t)v[0] << 18 | (uint32_t)v[1] << 12 | (uint32_t)v[2] << 6 | (uint32_t)v[3];
        int bytes = 3 - pad;
        if (n + (size_t)bytes > outlen)
            return -1;
        for (int k = 0; k < bytes; k++)
            out[n++] = (uint8_t)(bits >> (16 - 8 * k));
    }
    return (int)n;
}

// --- keys ---------------------------------------------------------------------------------------

static const struct {
    const char *type;
    uint16_t blob_len;      // exact wire-format length for this type
} key_types[] = {
    { "ssh-ed25519", 4 + 11 + 4 + 32 },
    { "ecdsa-sha2-nistp256", 4 + 19 + 4 + 8 + 4 + 65 },
};

static uint32_t get_u32be(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

const char *config_key_parse(const char *line, config_key_t *key) {
    memset(key, 0, sizeof(*key));
    while (*line == ' ' || *line == '\t')
        line++;

    size_t tlen = strcspn(line, " \t");
    const char *b64 = line + tlen;
    while (*b64 == ' ' || *b64 == '\t')
        b64++;
    size_t blen = strcspn(b64, " \t\r\n");
    if (tlen == 0 || blen == 0)
        return "expected '<type> <base64-key> [comment]'";

    int type = -1;
    for (unsigned i = 0; i < sizeof(key_types) / sizeof(key_types[0]); i++)
        if (strlen(key_types[i].type) == tlen && !memcmp(line, key_types[i].type, tlen))
            type = (int)i;
    if (type < 0)
        return "unsupported key type (use ssh-ed25519 or ecdsa-sha2-nistp256)";

    int n = config_base64_decode(b64, blen, key->blob, sizeof(key->blob));
    if (n < 0)
        return "key data is not valid base64";
    // The blob starts with its own type name, which must match the one given in front of it.
    if ((size_t)n != key_types[type].blob_len || get_u32be(key->blob) != tlen ||
        memcmp(key->blob + 4, line, tlen) != 0) {
        memset(key, 0, sizeof(*key));
        return "key data doesn't match its type";
    }
    key->len = (uint16_t)n;

    const char *comment = b64 + blen;
    while (*comment == ' ' || *comment == '\t')
        comment++;
    size_t clen = strcspn(comment, "\r\n");
    while (clen > 0 && (comment[clen - 1] == ' ' || comment[clen - 1] == '\t'))
        clen--;
    if (clen > CONFIG_KEY_COMMENT_MAX)
        clen = CONFIG_KEY_COMMENT_MAX;
    memcpy(key->comment, comment, clen);
    return NULL;
}

const char *config_key_type(const config_key_t *key, char *buf, size_t buflen) {
    if (key->len < 4 || buflen == 0)
        return "?";
    uint32_t tlen = get_u32be(key->blob);
    if (tlen == 0 || tlen > key->len - 4u || tlen >= buflen)
        return "?";
    memcpy(buf, key->blob + 4, tlen);
    buf[tlen] = '\0';
    return buf;
}

bool config_key_find(const sos_config_t *cfg, const uint8_t *blob, size_t len) {
    for (unsigned i = 0; i < cfg->key_count; i++)
        if (cfg->keys[i].len == len && !memcmp(cfg->keys[i].blob, blob, len))
            return true;
    return false;
}

const char *config_key_add(sos_config_t *cfg, const config_key_t *key) {
    if (key->len == 0 || key->len > CONFIG_KEY_BLOB_MAX)
        return "invalid key";
    if (config_key_find(cfg, key->blob, key->len))
        return "key is already trusted";
    if (cfg->key_count >= CONFIG_MAX_KEYS)
        return "key list is full (8 keys)";
    cfg->keys[cfg->key_count++] = *key;
    return NULL;
}

bool config_key_remove(sos_config_t *cfg, unsigned index) {
    if (index >= cfg->key_count)
        return false;
    memmove(&cfg->keys[index], &cfg->keys[index + 1],
            (cfg->key_count - index - 1) * sizeof(cfg->keys[0]));
    cfg->key_count--;
    memset(&cfg->keys[cfg->key_count], 0, sizeof(cfg->keys[0]));
    return true;
}
