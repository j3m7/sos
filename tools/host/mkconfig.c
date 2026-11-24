// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// sos-mkconfig <provision.conf> <out.bin>: see mkconfig.h and tools/provision.sh.

#include "mkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- parsing -------------------------------------------------------------------------------------

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t')
        s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        *--end = '\0';
    return s;
}

static const char *set_text(char *field, size_t size, const char *value,
                            const char *(*check)(const char *)) {
    const char *err = check(value);
    if (err)
        return err;
    snprintf(field, size, "%s", value);
    return NULL;
}

static const char *apply(sos_config_t *cfg, const char *name, const char *value) {
    if (!strcmp(name, "wifi_ssid"))
        return set_text(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), value, config_check_ssid);
    if (!strcmp(name, "wifi_password"))
        return set_text(cfg->wifi_password, sizeof(cfg->wifi_password), value,
                        config_check_password);
    if (!strcmp(name, "hostname"))
        return set_text(cfg->hostname, sizeof(cfg->hostname), value, config_check_hostname);
    if (!strcmp(name, "ssh_user"))
        return set_text(cfg->ssh_user, sizeof(cfg->ssh_user), value, config_check_user);
    if (!strcmp(name, "nts_server"))
        return set_text(cfg->ntp_server, sizeof(cfg->ntp_server), value, config_check_ntp_server);
    if (!strcmp(name, "ssh_key")) {
        config_key_t key;
        const char *err = config_key_parse(value, &key);
        return err ? err : config_key_add(cfg, &key);
    }
    if (!strcmp(name, "wifi_powersave")) {
        if (strcmp(value, "on") && strcmp(value, "off"))
            return "must be on or off";
        cfg->wifi_powersave = !strcmp(value, "on");
        return NULL;
    }
    if (!strcmp(name, "nts_sync_hours")) {
        char *end;
        unsigned long h = strtoul(value, &end, 10);
        if (!*value || *end || h > CONFIG_NTP_SYNC_HOURS_MAX)
            return "must be hours from 0 (off) to 720";
        cfg->ntp_sync_hours = (uint16_t)h;
        return NULL;
    }
    return "unknown setting";
}

const char *mkconfig_parse(char *text, sos_config_t *cfg, char *errbuf, size_t errlen) {
    config_defaults(cfg);
    unsigned lineno = 0;
    for (char *line = text; line && *line;) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        lineno++;
        char *s = trim(line);
        line = next;
        if (!*s || *s == '#')
            continue;
        char *eq = strchr(s, '=');
        if (!eq) {
            snprintf(errbuf, errlen, "line %u: expected 'name = value'", lineno);
            return errbuf;
        }
        *eq = '\0';
        const char *name = trim(s);
        const char *err = apply(cfg, name, trim(eq + 1));
        if (err) {
            snprintf(errbuf, errlen, "line %u: %s: %s", lineno, name, err);
            return errbuf;
        }
    }
    return NULL;
}

// --- image ---------------------------------------------------------------------------------------

static const uint8_t *image_read(void *ctx, unsigned slot) {
    return (const uint8_t *)ctx + slot * CONFIG_SLOT_SIZE;
}

static bool image_write(void *ctx, unsigned slot, const void *data, size_t len) {
    if (len > CONFIG_SLOT_SIZE)
        return false;
    uint8_t *p = (uint8_t *)ctx + slot * CONFIG_SLOT_SIZE;
    memset(p, 0xff, CONFIG_SLOT_SIZE);      // like the sector erase before programming
    memcpy(p, data, len);
    return true;
}

bool mkconfig_image(const sos_config_t *cfg, uint8_t image[MKCONFIG_IMAGE_SIZE]) {
    const config_store_t store = {image_read, image_write, image};
    sos_config_t back;
    memset(image, 0xff, MKCONFIG_IMAGE_SIZE);
    return config_save(&store, cfg) && config_load(&store, &back, NULL) &&
           !memcmp(&back, cfg, sizeof(back));
}

// --- command -------------------------------------------------------------------------------------

#ifndef MKCONFIG_NO_MAIN
int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <provision.conf> <out.bin>\n", argv[0]);
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    if (!in) {
        perror(argv[1]);
        return 1;
    }
    static char text[16384];
    size_t n = fread(text, 1, sizeof(text) - 1, in);
    bool too_big = !feof(in);
    fclose(in);
    if (too_big) {
        fprintf(stderr, "%s: too large\n", argv[1]);
        return 1;
    }
    text[n] = '\0';

    // Zeroed first: the struct's padding goes into the record, and the read-back compares it.
    static sos_config_t cfg;
    char err[160];
    if (mkconfig_parse(text, &cfg, err, sizeof(err))) {
        fprintf(stderr, "%s: %s\n", argv[1], err);
        return 1;
    }
    static uint8_t image[MKCONFIG_IMAGE_SIZE];
    if (!mkconfig_image(&cfg, image)) {
        fprintf(stderr, "building the configuration record failed\n");
        return 1;
    }
    FILE *out = fopen(argv[2], "wb");
    if (!out || fwrite(image, 1, sizeof(image), out) != sizeof(image) || fclose(out)) {
        perror(argv[2]);
        return 1;
    }
    printf("hostname %s, user %s, Wi-Fi %s (%s), %u trusted key(s)\n", cfg.hostname, cfg.ssh_user,
           cfg.wifi_ssid[0] ? cfg.wifi_ssid : "not set",
           cfg.wifi_password[0] ? "password set" : "open", cfg.key_count);
    return 0;
}
#endif
