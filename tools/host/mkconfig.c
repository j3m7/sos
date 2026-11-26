// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// sos-mkconfig <provision.conf> <out.bin>: see mkconfig.h and tools/provision.sh.

#include "mkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "time/timeconv.h"

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

// The Wi-Fi networks read so far: each wifi_ssid line starts one, and a wifi_password line
// belongs to the network before it (or to the first, if no wifi_ssid came yet).
typedef struct {
    config_wifi_net_t nets[CONFIG_WIFI_NETS];
    bool password_set[CONFIG_WIFI_NETS];
    unsigned count;
} wifi_list_t;

static const char *apply_wifi(wifi_list_t *w, const char *name, const char *value) {
    if (!strcmp(name, "wifi_ssid")) {
        const char *err = config_check_ssid(value);
        if (err)
            return err;
        for (unsigned i = 0; i < w->count; i++)
            if (!strcmp(w->nets[i].ssid, value))
                return "that network is already listed";
        if (w->count == CONFIG_WIFI_NETS)
            return "at most 5 networks";
        snprintf(w->nets[w->count].ssid, sizeof(w->nets[0].ssid), "%s", value);
        w->count++;
        return NULL;
    }
    unsigned i = w->count ? w->count - 1 : 0;
    if (w->password_set[i])
        return "a second wifi_password for one network (each wifi_ssid takes at most one)";
    w->password_set[i] = true;
    return set_text(w->nets[i].password, sizeof(w->nets[0].password), value, config_check_password);
}

static const char *apply(sos_config_t *cfg, const char *name, const char *value) {
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
    if (!strcmp(name, "console_password"))
        return set_text(cfg->console_password, sizeof(cfg->console_password), value,
                        config_check_console_password);
    if (!strcmp(name, "wifi_country"))
        return set_text(cfg->wifi_country, sizeof(cfg->wifi_country), value, config_check_country);
    if (!strcmp(name, "wifi_powersave")) {
        if (strcmp(value, "on") && strcmp(value, "off"))
            return "must be on or off";
        cfg->wifi_powersave = !strcmp(value, "on");
        return NULL;
    }
    if (!strcmp(name, "rtc_cal_ppm")) {
        int32_t mppm;
        int steps;
        if (!timeconv_parse_ppm(value, &mppm) || !timeconv_cal_steps(mppm, &steps))
            return "must be how fast the RTC runs in ppm, from -277 to 273";
        cfg->rtc_offset = (int8_t)steps;
        return NULL;
    }
    if (!strcmp(name, "glitch_detector")) {
        if (!strcmp(value, "off")) {
            cfg->glitch_detector = CONFIG_GLITCH_DETECTOR_OFF;
            return NULL;
        }
        if (!strcmp(value, "default")) {        // as with no line at all: armed at the highest
            cfg->glitch_detector = CONFIG_GLITCH_DETECTOR_DEFAULT;
            return NULL;
        }
        // "<n>" arms at sensitivity n; "watch <n>" only logs triggers.
        unsigned base = 1;
        if (!strncmp(value, "watch ", 6)) {
            base = 5;
            value += 6;
        }
        if (value[0] < '0' || value[0] > '3' || value[1])
            return "must be off, default, a sensitivity from 0 to 3, or 'watch' and a sensitivity";
        cfg->glitch_detector = (uint8_t)(value[0] - '0' + base);
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
    wifi_list_t wifi;
    memset(&wifi, 0, sizeof(wifi));
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
        const char *err = !strcmp(name, "wifi_ssid") || !strcmp(name, "wifi_password")
                              ? apply_wifi(&wifi, name, trim(eq + 1))
                              : apply(cfg, name, trim(eq + 1));
        if (err) {
            memset(&wifi, 0, sizeof(wifi));
            snprintf(errbuf, errlen, "line %u: %s: %s", lineno, name, err);
            return errbuf;
        }
    }
    if (!wifi.count && wifi.password_set[0]) {
        memset(&wifi, 0, sizeof(wifi));
        snprintf(errbuf, errlen, "wifi_password without a wifi_ssid");
        return errbuf;
    }
    for (unsigned i = 0; i < wifi.count; i++)
        config_wifi_add(cfg, wifi.nets[i].ssid, wifi.nets[i].password);    // checked above
    memset(&wifi, 0, sizeof(wifi));
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
    // No sealer: the image is a plain record, which an open board seals at its first boot.
    const config_store_t store = {image_read, image_write, image, NULL, false};
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
    printf("hostname %s, user %s, Wi-Fi %s (%s)", cfg.hostname, cfg.ssh_user,
           cfg.wifi_ssid[0] ? cfg.wifi_ssid : "not set",
           cfg.wifi_password[0] ? "password set" : "open");
    for (unsigned i = 1; i < config_wifi_count(&cfg); i++)
        printf(", then %s (%s)", cfg.wifi_extra[i - 1].ssid,
               cfg.wifi_extra[i - 1].password[0] ? "password set" : "open");
    printf(", %u trusted key(s)\n", cfg.key_count);
    return 0;
}
#endif
