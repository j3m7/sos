// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// src/core/config.c: the configuration record in flash. On an open board anyone with USB
// access can write a plain record (`ninja provision` does), so its contents are untrusted.
// Random bytes would almost never pass the CRC, so each slot is built here: a header with a
// known length chosen by the input and the right CRC, around payload bytes from the input.
// Then config_load() validates and loads it, and the loaded strings are read back to the end.
//
// config.c is included to get at the record's private layout (record_header_t, the lengths).

#include "../../src/core/config.c"  // NOLINT(bugprone-suspicious-include): see above

#include <stdlib.h>

#include "fuzz.h"

static uint8_t slots[2][CONFIG_SLOT_SIZE];

static const uint8_t *slot_read_fn(void *ctx, unsigned slot) {
    (void)ctx;
    return slots[slot];
}

static bool slot_write_fn(void *ctx, unsigned slot, const void *data, size_t len) {
    (void)ctx; (void)slot; (void)data; (void)len;
    return false;                                   // loading only
}

// One slot from the input: a selector byte (length, version, whether the CRC is right), a
// sequence number, then the payload.
static void build_slot(uint8_t *slot, const uint8_t **data, size_t *size) {
    memset(slot, 0xff, CONFIG_SLOT_SIZE);
    if (!*size)
        return;                                     // erased
    uint8_t sel = fuzz_byte(data, size);
    size_t nlengths = sizeof(record_lengths) / sizeof(record_lengths[0]);
    record_header_t h = {
        .magic = CONFIG_MAGIC,
        .version = (sel & 0x40) ? CONFIG_VERSION_SEALED : CONFIG_VERSION_PLAIN,
        .length = record_lengths[(sel & 0x3f) % nlengths],
    };
    for (int i = 0; i < 4; i++)
        h.seq = h.seq << 8 | fuzz_byte(data, size);
    size_t n = *size < h.length ? *size : h.length;
    memcpy(slot + HEADER_LEN, *data, n);            // the rest stays 0xff
    *data += n;
    *size -= n;
    memcpy(slot, &h, HEADER_LEN);
    h.crc = record_crc(slot, h.length);
    if (sel & 0x80)
        h.crc ^= 1;                                 // sometimes wrong, to take that path too
    memcpy(slot, &h, HEADER_LEN);
}

static size_t len_of(const char *s, size_t size) {
    size_t n = strlen(s);                           // runs off the field if not terminated
    if (n >= size)
        abort();
    return n;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    build_slot(slots[0], &data, &size);
    build_slot(slots[1], &data, &size);
    const config_store_t store = {slot_read_fn, slot_write_fn, NULL, NULL, false};
    config_stored(&store);
    sos_config_t cfg;
    uint32_t seq;
    if (!config_load(&store, &cfg, &seq))
        return 0;
    // What the firmware then uses as C strings must be terminated inside its field.
    len_of(cfg.wifi_ssid, sizeof(cfg.wifi_ssid));
    len_of(cfg.wifi_password, sizeof(cfg.wifi_password));
    len_of(cfg.hostname, sizeof(cfg.hostname));
    len_of(cfg.ssh_user, sizeof(cfg.ssh_user));
    len_of(cfg.ntp_server, sizeof(cfg.ntp_server));
    len_of(cfg.wifi_country, sizeof(cfg.wifi_country));
    len_of(cfg.console_password, sizeof(cfg.console_password));
    if (cfg.key_count > CONFIG_MAX_KEYS)
        abort();
    for (unsigned i = 0; i < cfg.key_count; i++)
        len_of(cfg.keys[i].comment, sizeof(cfg.keys[i].comment));
    config_wifi_net_t nets[CONFIG_WIFI_NETS];
    unsigned n = wifi_unpack(&cfg, nets);
    for (unsigned i = 0; i < n; i++) {
        len_of(nets[i].ssid, sizeof(nets[i].ssid));
        len_of(nets[i].password, sizeof(nets[i].password));
    }
    return 0;
}
