// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez
//
// tools/host/mkconfig.c: the provisioning file parser and the config image it produces, checked
// by loading the image the way the device does.

#include "mkconfig.h"

#include "test.h"

#define KEY "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIDj0pL9cW2vAC8YN5tBpXkS1x0rG0jB1q6VZ8kq2m3Yd me@host"

static const uint8_t *rd(void *ctx, unsigned slot) {
    return (const uint8_t *)ctx + slot * CONFIG_SLOT_SIZE;
}
static bool wr(void *ctx, unsigned slot, const void *d, size_t n) {
    (void)ctx; (void)slot; (void)d; (void)n;
    return false;
}

static const char *parse(const char *text, sos_config_t *cfg) {
    static char buf[2048], err[160];
    snprintf(buf, sizeof(buf), "%s", text);
    memset(cfg, 0, sizeof(*cfg));
    return mkconfig_parse(buf, cfg, err, sizeof(err));
}

TEST(full_file_loads_like_on_the_device) {
    sos_config_t cfg, back;
    CHECK(!parse("# a board\n"
                 "\n"
                 "  wifi_ssid = My Network  \r\n"
                 "wifi_password=correct horse\n"
                 "wifi_powersave = on\n"
                 "wifi_country = US\n"
                 "hostname = bench1\n"
                 "ssh_user = dev\n"
                 "ssh_key = " KEY "\n"
                 "nts_server = time.example.org\n"
                 "nts_sync_hours = 24\n"
                 "rtc_cal_ppm = 92\n"
                 "temp_cal = 1.8\n", &cfg));
    static uint8_t image[MKCONFIG_IMAGE_SIZE];
    CHECK(mkconfig_image(&cfg, image));
    const config_store_t store = {rd, wr, image, NULL, false};
    uint32_t seq = 0;
    CHECK(config_load(&store, &back, &seq));
    CHECK_EQ(seq, 1);
    CHECK(!strcmp(back.wifi_ssid, "My Network"));       // inner space kept, ends trimmed
    CHECK(!strcmp(back.wifi_password, "correct horse"));
    CHECK_EQ(back.wifi_powersave, 1);
    CHECK(!strcmp(back.wifi_country, "US"));
    CHECK(!strcmp(back.hostname, "bench1"));
    CHECK(!strcmp(back.ssh_user, "dev"));
    CHECK_EQ(back.key_count, 1);
    CHECK(!strcmp(back.keys[0].comment, "me@host"));
    CHECK(!strcmp(back.ntp_server, "time.example.org"));
    CHECK_EQ(back.ntp_sync_hours, 24);
    CHECK_EQ(back.rtc_offset, 21);                      // 92 ppm fast: 21 steps of 4.34 ppm
    CHECK_EQ(back.temp_offset, 18);                     // temp_cal = 1.8
    // The second slot is erased, so nothing the board had saved survives.
    for (size_t i = CONFIG_SLOT_SIZE; i < MKCONFIG_IMAGE_SIZE; i++)
        if (image[i] != 0xff) {
            CHECK(0 && "second slot not erased");
            break;
        }
}

TEST(empty_file_gives_defaults) {
    sos_config_t cfg;
    CHECK(!parse("# nothing\n", &cfg));
    CHECK(!strcmp(cfg.hostname, "sos"));
    CHECK(!strcmp(cfg.ssh_user, "admin"));
    CHECK_EQ(cfg.key_count, 0);
    CHECK_EQ(cfg.wifi_ssid[0], 0);
}

TEST(errors_name_the_line) {
    sos_config_t cfg;
    const char *e = parse("hostname = ok\nwifi_password = short\n", &cfg);
    CHECK(e && strstr(e, "line 2") && strstr(e, "wifi_password"));
    e = parse("colour = blue\n", &cfg);
    CHECK(e && strstr(e, "unknown setting"));
    e = parse("just some words\n", &cfg);
    CHECK(e && strstr(e, "line 1"));
    e = parse("ssh_key = ssh-ed25519 AAAA\n", &cfg);
    CHECK(e && strstr(e, "ssh_key"));
    e = parse("ssh_key = " KEY "\nssh_key = " KEY "\n", &cfg);
    CHECK(e && strstr(e, "line 2") && strstr(e, "already"));
    e = parse("wifi_powersave = maybe\n", &cfg);
    CHECK(e != NULL);
    e = parse("wifi_country = usa\n", &cfg);
    CHECK(e != NULL);
    e = parse("nts_sync_hours = 9999\n", &cfg);
    CHECK(e != NULL);
    e = parse("rtc_cal_ppm = 300\n", &cfg);
    CHECK(e != NULL);
    e = parse("rtc_cal_ppm = fast\n", &cfg);
    CHECK(e != NULL);
    CHECK(!parse("rtc_cal_ppm = -12.5\n", &cfg));
    CHECK_EQ(cfg.rtc_offset, -3);
    CHECK(!parse("temp_cal = -2.4\n", &cfg));
    CHECK_EQ(cfg.temp_offset, -24);
    CHECK(!parse("temp_cal = 15\n", &cfg));
    CHECK_EQ(cfg.temp_offset, 150);
    e = parse("temp_cal = 15.1\n", &cfg);
    CHECK(e != NULL);
    e = parse("temp_cal = warm\n", &cfg);
    CHECK(e != NULL);
    e = parse("hostname = bad name\n", &cfg);
    CHECK(e != NULL);
}

// console_password travels in the plain image (the board turns it into a verifier when it
// seals the record); a bad one is refused with its line number.
TEST(console_password_line) {
    sos_config_t cfg, back;
    CHECK(!parse("wifi_ssid = net\nconsole_password = open sesame 123\n", &cfg));
    CHECK(!strcmp(cfg.console_password, "open sesame 123"));
    static uint8_t image[MKCONFIG_IMAGE_SIZE];
    CHECK(mkconfig_image(&cfg, image));
    const config_store_t store = {rd, wr, image, NULL, false};
    CHECK(config_load(&store, &back, NULL));
    CHECK(!strcmp(back.console_password, "open sesame 123"));
    CHECK_EQ(back.console_iterations, 0);               // no verifier: the tool can't make one
    const char *err = parse("wifi_ssid = net\nconsole_password = short\n", &cfg);
    CHECK(err != NULL && strstr(err, "line 2") != NULL);
    // Without the line there's no password in the image.
    CHECK(!parse("wifi_ssid = net\n", &cfg));
    CHECK_EQ(cfg.console_password[0], '\0');
}

// tz = <name> is looked up in tz/zones.bundle, as on the board, and both fields go into the image.
TEST(time_zone_line) {
    sos_config_t cfg, back;
    mkconfig_set_bundle(TZ_BUNDLE);
    CHECK(!parse("wifi_ssid = net\ntz = europe/madrid\n", &cfg));
    CHECK(!strcmp(cfg.tz_name, "Europe/Madrid"));
    CHECK(!strcmp(cfg.tz_rule, "CET-1CEST,M3.5.0,M10.5.0/3"));
    static uint8_t image[MKCONFIG_IMAGE_SIZE];
    CHECK(mkconfig_image(&cfg, image));
    const config_store_t store = {rd, wr, image, NULL, false};
    CHECK(config_load(&store, &back, NULL));
    CHECK(!strcmp(back.tz_name, "Europe/Madrid"));
    CHECK(!strcmp(back.tz_rule, "CET-1CEST,M3.5.0,M10.5.0/3"));
    CHECK(!parse("tz = Europe/Madrid\ntz = UTC\n", &cfg));
    CHECK_EQ(cfg.tz_name[0], '\0');
    const char *err = parse("wifi_ssid = net\ntz = Europe/Madird\n", &cfg);
    CHECK(err && strstr(err, "line 2: tz: no zone Europe/Madird") &&
          strstr(err, "did you mean Europe/Madrid"));
    err = parse("tz = A234567890123456789012345678901234567890\n", &cfg);
    CHECK(err && strstr(err, "that long"));
    mkconfig_set_bundle(NULL);
    err = parse("tz = Europe/Madrid\n", &cfg);
    CHECK(err && strstr(err, "can't be read"));
    CHECK(!parse("wifi_ssid = net\n", &cfg));            // no tz line: no bundle needed
    CHECK_EQ(cfg.tz_name[0], '\0');
}

TEST(glitch_detector_line) {
    sos_config_t cfg;
    CHECK(!parse("wifi_ssid = net\n", &cfg));
    CHECK_EQ(cfg.glitch_detector, CONFIG_GLITCH_DETECTOR_DEFAULT);  // no line: the default
    CHECK(!parse("wifi_ssid = net\nglitch_detector = default\n", &cfg));
    CHECK_EQ(cfg.glitch_detector, CONFIG_GLITCH_DETECTOR_DEFAULT);
    CHECK(!parse("wifi_ssid = net\nglitch_detector = 0\n", &cfg));
    CHECK_EQ(cfg.glitch_detector, 1);                   // sensitivity 0 is setting 1
    CHECK(!parse("wifi_ssid = net\nglitch_detector = 3\n", &cfg));
    CHECK_EQ(cfg.glitch_detector, 4);
    CHECK(!parse("wifi_ssid = net\nglitch_detector = off\n", &cfg));
    CHECK_EQ(cfg.glitch_detector, CONFIG_GLITCH_DETECTOR_OFF);
    CHECK(!parse("wifi_ssid = net\nglitch_detector = watch 0\n", &cfg));
    CHECK_EQ(cfg.glitch_detector, 5);
    CHECK(!parse("wifi_ssid = net\nglitch_detector = watch 3\n", &cfg));
    CHECK_EQ(cfg.glitch_detector, 8);
    CHECK(parse("wifi_ssid = net\nglitch_detector = watch 4\n", &cfg) != NULL);
    CHECK(parse("wifi_ssid = net\nglitch_detector = 4\n", &cfg) != NULL);
    CHECK(parse("wifi_ssid = net\nglitch_detector = on\n", &cfg) != NULL);
}

TEST(several_wifi_networks_in_order) {
    sos_config_t cfg, back;
    CHECK(!parse("wifi_ssid = home\n"
                 "wifi_password = correct horse\n"
                 "hostname = bench1\n"
                 "wifi_ssid = phone\n"                  // open: no password line
                 "wifi_ssid = work\n"
                 "wifi_password = battery staple\n", &cfg));
    static uint8_t image[MKCONFIG_IMAGE_SIZE];
    CHECK(mkconfig_image(&cfg, image));
    const config_store_t store = {rd, wr, image, NULL, false};
    CHECK(config_load(&store, &back, NULL));
    CHECK_EQ(config_wifi_count(&back), 3);
    CHECK(!strcmp(back.wifi_ssid, "home"));
    CHECK(!strcmp(back.wifi_password, "correct horse"));
    CHECK(!strcmp(back.wifi_extra[0].ssid, "phone"));
    CHECK_EQ(back.wifi_extra[0].password[0], 0);
    CHECK(!strcmp(back.wifi_extra[1].ssid, "work"));
    CHECK(!strcmp(back.wifi_extra[1].password, "battery staple"));
}

// As files written for one network had it: the password may come first.
TEST(password_before_the_ssid) {
    sos_config_t cfg;
    CHECK(!parse("wifi_password = correct horse\nwifi_ssid = home\n", &cfg));
    CHECK(!strcmp(cfg.wifi_ssid, "home"));
    CHECK(!strcmp(cfg.wifi_password, "correct horse"));
}

TEST(bad_wifi_lists_name_the_line) {
    sos_config_t cfg;
    const char *e = parse("wifi_ssid = a\nwifi_ssid = a\n", &cfg);
    CHECK(e && strstr(e, "line 2") && strstr(e, "already listed"));
    e = parse("wifi_ssid = a\nwifi_password = 12345678\nwifi_password = 87654321\n", &cfg);
    CHECK(e && strstr(e, "line 3") && strstr(e, "second wifi_password"));
    e = parse("wifi_ssid = a\nwifi_ssid = b\nwifi_ssid = c\nwifi_ssid = d\nwifi_ssid = e\n"
              "wifi_ssid = f\n", &cfg);
    CHECK(e && strstr(e, "line 6") && strstr(e, "at most 5"));
    e = parse("wifi_password = 12345678\n", &cfg);
    CHECK(e && strstr(e, "without a wifi_ssid"));
}

TEST_MAIN(
    RUN(time_zone_line);
    RUN(several_wifi_networks_in_order);
    RUN(password_before_the_ssid);
    RUN(bad_wifi_lists_name_the_line);
    RUN(glitch_detector_line);
    RUN(console_password_line);
    RUN(full_file_loads_like_on_the_device);
    RUN(empty_file_gives_defaults);
    RUN(errors_name_the_line);
)
