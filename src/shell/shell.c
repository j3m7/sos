// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include "shell/shell.h"
#include "shell/words.h"
#include "shell/login.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hardware/regs/powman.h"
#include "hardware/structs/powman.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "lwip/memp.h"
#include "lwip/netif.h"
#include "lwip/stats.h"

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/hash.h>

#include "chat/chat.h"
#include "core/bootkey.h"
#include "core/chipinfo.h"
#include "core/config.h"
#include "core/failflash.h"
#include "core/fault.h"
#include "core/flashop.h"
#include "core/glitchdet.h"
#include "core/resetwhy.h"
#include "core/health.h"
#include "core/statecheck.h"
#include "core/heap.h"
#include "core/heaptag.h"
#include "core/history.h"
#include "core/bgcheck_board.h"
#include "core/countcheck.h"
#include "core/countcheck_board.h"
#include "core/fullcheck_board.h"
#include "core/log.h"
#ifdef SOS_TEST_COMMANDS
#include "core/memhold.h"
#endif
#include "core/logfile.h"
#include "core/otpread.h"
#include "core/rootflash.h"
#include "core/rootsecret.h"
#include "core/rootsource.h"
#include "core/consoleonly.h"
#include "core/sensors.h"
#include "core/settings.h"
#include "core/stackmon.h"
#include "core/version.h"
#include "core/wdog.h"
#include "edit/edit.h"
#include "man/man.h"
#include "files/files.h"
#include "files/gzip.h"
#include "files/sdcard.h"
#include "files/sums.h"
#include "files/tar.h"
#include "lua/lua_core1.h"
#include "lua/lua_host.h"
#include "net/fetch.h"
#include "net/http.h"
#include "net/ping.h"
#include "net/wifi.h"
#include "net/wifistat.h"
#include "ssh/hostkey.h"
#include "ssh/sshd.h"
#include "time/nts.h"
#include "time/timeconv.h"
#include "time/timesync.h"
#include "time/tzzone.h"
#include "tls/certstore.h"

// --- helpers -------------------------------------------------------------------------------------

// Configuration changes need physical access: only the console channel is an admin channel.
// (So the multi-step flows below, which keep their state in statics, only ever run there.)
static bool require_admin(shell_t *sh) {
    if (sh->io->admin)
        return true;
    shell_printf(sh, "configuration can only be changed on the serial console\n");
    return false;
}

static void save(shell_t *sh) {
    if (settings_save())
        return;
    if (settings_save_refused())
        shell_printf(sh, SETTINGS_REFUSED_FMT "the change holds only until a reboot\n",
                     settings_save_refused());
    else
        shell_printf(sh, "error: saving to flash failed; the change will be lost at reboot\n");
}

bool shell_require_admin(shell_t *sh) {
    return require_admin(sh);
}

void shell_save_settings(shell_t *sh) {
    save(sh);
}

// --- information ---------------------------------------------------------------------------------

// The commit, then which board and chip this is. The serial is the one the board shows over USB
// (and to picotool); the chip revision decides which published attacks apply (issue #64).
static void cmd_version(shell_t *sh) {
    char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1], chip[64];
    pico_get_unique_board_id_string(serial, sizeof(serial));
    chipinfo_format(chip, sizeof(chip), rp2350_chip_version(), rp2350_rom_version());
    shell_printf(sh, "SOS %s\n", sos_version);
#ifdef SOS_BURN_IMAGE
    shell_printf(sh, "THIS IS THE BURN IMAGE: it can write OTP (`rootsecret burn`). Not for "
                     "everyday use.\n");
#endif
    shell_printf(sh, "board: serial %s\n", serial);
    shell_printf(sh, "chip: %s\n", chip);
    otpread_status_t otp;
    otpread_status(&otp);
    shell_printf(sh, "secure boot: %s\n", otp.secure_boot
                     ? "on (only firmware signed with a boot key in OTP starts)"
                     : "off (open board: any firmware starts)");
    shell_printf(sh, "debug port: %s\n", otp.debug_disabled ? "disabled"
                     : otp.secure_debug_disabled ? "Secure debug disabled" : "enabled");
    shell_printf(sh, "root secret: %s\n", rootsource() == ROOTSRC_OTP ? "in OTP"
                     : rootsource() == ROOTSRC_FLASH ? "in flash (open board)" : "none");
    switch (consoleonly_state()) {
    case CONSOLEONLY_NO:
        shell_printf(sh, "console-only setup: no (a settings file written to flash over USB is "
                         "accepted, as `ninja provision` writes it)\n");
        break;
    case CONSOLEONLY_YES:
        shell_printf(sh, "console-only setup: yes%s (settings files written over USB are refused; "
                         "set up on the console only)\n",
                     consoleonly_forced() ? ", forced by this test build, not by the board" : "");
        break;
    case CONSOLEONLY_YES_UNREADABLE:
        shell_printf(sh, "console-only setup: yes, because its marker in OTP can't be read\n");
        break;
    case CONSOLEONLY_YES_DAMAGED:
        shell_printf(sh, "console-only setup: yes, because its marker in OTP is neither blank nor "
                         "whole (rows 0x%06lx 0x%06lx 0x%06lx)\n",
                     (unsigned long)consoleonly_rows()[0], (unsigned long)consoleonly_rows()[1],
                     (unsigned long)consoleonly_rows()[2]);
        break;
    }
    // Secure boot and debug are what the chip latched at reset. Its OTP rows should say the same.
    if (!otp.rows_readable)
        shell_printf(sh, "(the OTP rows behind those flags are unreadable, so they couldn't be "
                         "cross-checked)\n");
    else if (!otp.rows_agree)
        shell_printf(sh, "WARNING: the OTP rows say otherwise (rows 0x%02lx, chip 0x%02lx): "
                         "written since the last reset, or damaged\n",
                     (unsigned long)(otp.rows & 0x7f), (unsigned long)(otp.latched & 0x7f));
}

// --- boot key --------------------------------------------------------------------------------

static void print_hash(shell_t *sh, const uint8_t *hash) {
    char hex[2 * BOOTKEY_HASH_LEN + 1];
    for (unsigned i = 0; i < BOOTKEY_HASH_LEN; i++)
        snprintf(hex + 2 * i, 3, "%02x", hash[i]);
    shell_printf(sh, "%s", hex);
}

// `bootkey`: would this firmware still start with secure boot on? It finds the public key the
// running image is signed with, hashes it as the boot ROM does, and compares that with the boot
// key slots in OTP. Reads only. The runbooks (docs/security/README.md) run it before the bit
// that turns secure boot on is written, because a wrong hash in OTP then means a board that
// runs nothing.
static void cmd_bootkey(shell_t *sh) {
    extern char __flash_binary_start, __flash_binary_end;
    // The signature's block is added after linking, just past what the linker calls the end.
    const uint8_t *image = (const uint8_t *)&__flash_binary_start;
    uintptr_t start = (uintptr_t)&__flash_binary_start, end = (uintptr_t)&__flash_binary_end;
    size_t len = (size_t)(end - start) + 4096;
    size_t flash_left = XIP_BASE + PICO_FLASH_SIZE_BYTES - (uintptr_t)image;
    if (len > flash_left)
        len = flash_left;

    const uint8_t *pubkey;
    bootkey_image_t kind = bootkey_image_pubkey(image, len, &pubkey);
    uint8_t hash[BOOTKEY_HASH_LEN];
    bool signed_image = kind == BOOTKEY_IMAGE_SIGNED &&
                        wc_Sha256Hash(pubkey, BOOTKEY_PUBKEY_LEN, hash) == 0;
    if (signed_image) {
        shell_printf(sh, "this firmware: signed; its key's hash is ");
        print_hash(sh, hash);
        shell_printf(sh, "\n");
    } else if (kind == BOOTKEY_IMAGE_UNSIGNED) {
        shell_printf(sh, "this firmware: not signed\n");
    } else if (kind == BOOTKEY_IMAGE_SIGNED) {
        shell_printf(sh, "this firmware: signed, but hashing its key failed\n");
    } else {
        shell_printf(sh, "this firmware: its image description wasn't found in flash, so "
                         "nothing can be said about its signature\n");
    }

    bootkey_otp_t otp;
    otpread_status_t st;
    otpread_status(&st);
    shell_printf(sh, "secure boot: %s\n", st.secure_boot ? "on" : "off");
    if (!otpread_bootkeys(&otp)) {
        shell_printf(sh, "boot key slots in OTP: unreadable\n"
                         "NO VERDICT: the boot keys can't be read from here\n");
        return;
    }
    bool any = false;
    for (unsigned s = 0; s < BOOTKEY_SLOTS; s++) {
        bool blank = bootkey_slot_blank(&otp, s);
        bool marked_valid = otp.valid >> s & 1u, marked_invalid = otp.invalid >> s & 1u;
        if (blank && !marked_valid && !marked_invalid)
            continue;
        any = true;
        shell_printf(sh, "boot key slot %u: ", s);
        if (blank)
            shell_printf(sh, "(blank)");
        else
            print_hash(sh, otp.slot[s]);
        shell_printf(sh, "  %s\n", marked_invalid ? "marked invalid: never accepted"
                                   : marked_valid ? "valid" : "not marked valid");
    }
    if (!any)
        shell_printf(sh, "boot key slots in OTP: all four blank\n");

    if (!signed_image) {
        if (kind == BOOTKEY_IMAGE_UNSIGNED)
            shell_printf(sh, "NO MATCH: %s\n", st.secure_boot
                             ? "(unexpected: secure boot is on and this firmware is running)"
                             : "with secure boot on, this firmware would not start");
        else
            shell_printf(sh, "NO VERDICT: this firmware's key couldn't be determined\n");
        return;
    }
    int slot;
    switch (bootkey_compare(&otp, hash, &slot)) {
    case BOOTKEY_MATCH:
        shell_printf(sh, "MATCH: slot %d holds the hash of this firmware's key and is valid. "
                         "This compares keys only:\nthe signature itself isn't checked here "
                         "(`picotool info -a` on the image says `signature: verified`)\n", slot);
        break;
    case BOOTKEY_MATCH_NOT_VALID:
        shell_printf(sh, "NOT YET: slot %d holds this firmware's key but isn't marked valid. "
                         "With secure boot on, this firmware would not start\n", slot);
        break;
    case BOOTKEY_MATCH_INVALIDATED:
        shell_printf(sh, "NO MATCH: slot %d holds this firmware's key but is marked invalid. "
                         "With secure boot on, this firmware would not start\n", slot);
        break;
    default:
        shell_printf(sh, "NO MATCH: no boot key slot holds this firmware's key. With secure boot "
                         "on, this firmware would not start\n");
    }
}

static void cmd_help(shell_t *sh) {
    shell_printf(sh, "help              this list\n");
    shell_printf(sh, "uptime [history]  time since boot, and why it last reset; recorded boots\n");
    shell_printf(sh, "temp              chip temperature, lowest and highest since boot\n");
    shell_printf(sh, "temp cal          chip temperature calibration\n");
    shell_printf(sh, "power             supply voltage (VSYS) and USB power\n");
    shell_printf(sh, "version           firmware commit, board serial, chip revision, secure boot\n");
    shell_printf(sh, "time [sync]       date and time (UTC) / set clock and RTC from NTS\n");
    shell_printf(sh, "time auto|server  periodic sync / NTS server settings\n");
    shell_printf(sh, "time cal [ppm]    RTC calibration: how fast the RTC runs\n");
    shell_printf(sh, "nts [ke]          NTS status / key exchange (development)\n");
    shell_printf(sh, "nts query         authenticated NTS time query; reports the offset\n");
    shell_printf(sh, "tls roots         trusted root certificates (built-in and SD card)\n");
    shell_printf(sh, "mem               heap (by feature) and stack usage\n");
    shell_printf(sh, "net               Wi-Fi and IP status\n");
    shell_printf(sh, "ping <host> [n]   is the host reachable? n echo requests, default 4 (Ctrl-C stops)\n");
    shell_printf(sh, "ssh               SSH server status\n");
    shell_printf(sh, "who               who is logged in, and who is in the chat\n");
    shell_printf(sh, "wall <message>    send a message to every other shell\n");
    shell_printf(sh, "chat              talk with the others in the chat room (Ctrl-D leaves)\n");
    shell_printf(sh, "dmesg [-f <fac>]  system log, or one facility's (kern auth daemon user)\n");
    shell_printf(sh, "logger [-p <level>] <message>  add a line to the log\n");
    shell_printf(sh, "log               log levels\n");
    shell_printf(sh, "config            show the configuration\n");
    shell_printf(sh, "glitch            glitch detector setting, state and resets\n");
    shell_printf(sh, "health            self-checks: the latest result of each\n");
    shell_printf(sh, "rootsecret        the root secret that stored keys are derived from\n");
    shell_printf(sh, "bootkey           would this firmware start with secure boot on? (its key vs OTP)\n");
    shell_printf(sh, "key [list]        trusted SSH public keys\n");
    shell_printf(sh, "wifi              Wi-Fi networks and state\n");
    shell_printf(sh, "wifi stats [chip] signal, noise, packet and pool counters [and the chip's]\n");
    shell_printf(sh, "sd [info]         SD card status (also: sd speed)\n");
    files_help(sh);
    gzip_help(sh);
    tar_help(sh);
    shell_printf(sh, "edit <file>       full-screen text editor (Ctrl-S saves, Ctrl-Q quits)\n");
    shell_printf(sh, "more <file>       read a file a screen at a time (q quits, h lists the keys)\n");
    shell_printf(sh, "<command> | more  read any command's output the same way (dmesg | more)\n");
    shell_printf(sh, "<command> | grep [-v] [-i] [-n] <text>  only the lines with the text\n");
    shell_printf(sh, "<command> | head|tail [<n>]  the first / last n lines (10); | wc counts\n");
    shell_printf(sh, "<command> | tee [-a] <file>  also write the output to a file (-a appends)\n");
    shell_printf(sh, "                  stages chain: dmesg | grep wifi | tail 5 | more\n");
    shell_printf(sh, "man [<topic>]     the manual: list pages or read one (man install fetches them)\n");
    shell_printf(sh, "lua <file> | lua -e <code>  run a Lua script (on core 1; Ctrl-C stops)\n");
    shell_printf(sh, "lua               interactive Lua prompt (Ctrl-D or exit leaves)\n");
    shell_printf(sh, "lua -b <file>     run a script in the background (output to the log)\n");
    shell_printf(sh, "lua ps | lua stop the running script / stop it\n");
    shell_printf(sh, "http get|head <url>, http post <url> <text>  HTTPS request (testing)\n");
    shell_printf(sh, "fetch [-f] <url> [<file>]  download to a file (Ctrl-C stops)\n");
    if (sh->io->admin) {
        shell_printf(sh, "setup             guided configuration\n");
        shell_printf(sh, "passwd            set or change the console password\n");
        shell_printf(sh, "glitch watch|on <0-3>, glitch off|default  glitch detector: log triggers / reset on them (default: on 3)\n");
        shell_printf(sh, "log console|history|file <level>  set log levels (until reboot)\n");
        shell_printf(sh, "log rotate [every <N>d|at <HH:MM>]  rotate the log files now / when\n");
        shell_printf(sh, "time sync sntp    one unauthenticated SNTP sync (" SOS_NTP_SERVER ")\n");
        shell_printf(sh, "nts ke <host>     key exchange with another server (development)\n");
        shell_printf(sh, "wifi set|connect  change the first network / reconnect\n");
        shell_printf(sh, "wifi add|remove <n>|prefer <n>  more networks: the first in reach is used\n");
        shell_printf(sh, "wifi power on|off Wi-Fi power saving (default off)\n");
        shell_printf(sh, "wifi country <XX>|worldwide  radio rules to follow, e.g. US (after reboot)\n");
        shell_printf(sh, "wifi keepalive on|off  testing: stop announcing the address (until reboot)\n");
        shell_printf(sh, "wifi iovar <name> [<value>]  debugging: read or set a Wi-Fi chip variable\n");
        shell_printf(sh, "key add [line]    trust a public key (paste an authorized_keys line)\n");
        shell_printf(sh, "key del <n>       stop trusting key n\n");
        shell_printf(sh, "hostname <name>   set the hostname\n");
        shell_printf(sh, "time auto <h>|off sync the clock from NTS every h hours (default off)\n");
        shell_printf(sh, "time server <name>|default  set the NTS server\n");
        shell_printf(sh, "tls roots approve|forget  trust /certs/roots.pem for TLS / stop\n");
        shell_printf(sh, "config reset      erase the configuration\n");
        shell_printf(sh, "crash fault|stack|panic|hang  testing: stop the firmware that way\n");
        shell_printf(sh, "sd format [exfat|fat32] [plain]\n");
        shell_printf(sh, "                  erase and format the SD card: encrypted unless plain,\n");
        shell_printf(sh, "                  showing the card's key once, to store offline\n");
        shell_printf(sh, "sd unmount        unmount the card, to remove it\n");
        shell_printf(sh, "sd check          check the unmounted card's file system (reads only)\n");
        shell_printf(sh, "sd test [KB [keep]]  write/read benchmark (everything else waits)\n");
        shell_printf(sh, "health run [<name>]|full  run the self-checks now (full: the slow ones too)\n");
    }
    shell_printf(sh, "logout            end this session (also Ctrl-D)\n");
    shell_printf(sh, "reboot            restart the device\n");
}

static void cmd_uptime(shell_t *sh) {
    uint64_t s = to_us_since_boot(get_absolute_time()) / 1000000;
    // The reason (core/resetwhy.h); a fault's record gets a line of its own (it's long).
    bool fault = resetwhy() == RESETWHY_FAULT;
    shell_printf(sh, "up %llud %02llu:%02llu:%02llu (last reset: %s)\n",
                 s / 86400, (s / 3600) % 24, (s / 60) % 60, s % 60,
                 fault ? "fault" : resetwhy_text());
    if (fault)
        shell_printf(sh, "last reset: %s\n", resetwhy_text());
    // Each flash write stops everything else for as long as it takes (core/flashop.h).
    flashop_stats_t f = flashop_stats();
    if (f.count)
        shell_printf(sh, "flash writes: %lu, interrupts off %lu.%lu ms for the last, %lu.%lu ms "
                         "for the longest\n",
                     (unsigned long)f.count, (unsigned long)(f.last_us / 1000),
                     (unsigned long)(f.last_us / 100 % 10), (unsigned long)(f.max_us / 1000),
                     (unsigned long)(f.max_us / 100 % 10));
}

// "since boot: lowest ... (... ago), highest ... (... ago), read every 10 s", from a reading's
// stats and its text function (core/sensors.h).
static void print_since_boot(shell_t *sh, const sensors_stats_t *st,
                             void (*format)(char *, size_t, int)) {
    if (!st->count)
        return;
    char lo[SENSORS_TEXT_MAX], hi[SENSORS_TEXT_MAX], lo_age[24], hi_age[24];
    format(lo, sizeof(lo), st->min);
    format(hi, sizeof(hi), st->max);
    sensors_format_age(lo_age, sizeof(lo_age), st->last_s - st->min_s);
    sensors_format_age(hi_age, sizeof(hi_age), st->last_s - st->max_s);
    shell_printf(sh, "since boot: lowest %s (%s ago), highest %s (%s ago), read every %u s\n",
                 lo, lo_age, hi, hi_age, SENSORS_SAMPLE_S);
}

// A temperature difference with its sign: "+1.8 C", "-0.4 C", "0.0 C".
static void format_temp_delta(char *out, size_t size, int tenths) {
    char t[SENSORS_TEXT_MAX];
    sensors_format_temp(t, sizeof(t), tenths);
    snprintf(out, size, "%s%s", tenths > 0 ? "+" : "", t);
}

// `temp cal [off | <degrees>]`: the chip temperature's calibration (a setting, so console only
// to change). <degrees> is what a reference thermometer beside the board reads now; the
// difference from the sensor's own reading is stored and added to every reading after.
static void cmd_temp_cal(shell_t *sh, const char *value) {
    char d[SENSORS_TEXT_MAX + 1], r[SENSORS_TEXT_MAX], ref_t[SENSORS_TEXT_MAX];
    int old = settings()->temp_offset;
    while (*value == ' ')
        value++;
    if (!*value) {
        format_temp_delta(d, sizeof(d), old);
        if (old)
            shell_printf(sh, "chip temperature calibration: %s added to the sensor's reading\n", d);
        else
            shell_printf(sh, "chip temperature calibration: none (uncalibrated)\n");
        return;
    }
    if (!require_admin(sh))
        return;
    int ref = 0, raw = 0, offset = 0;
    if (strcmp(value, "off")) {
        if (!sensors_parse_temp(value, &ref)) {
            shell_printf(sh, "not changed: give 'off', or what a thermometer beside the board "
                             "reads now, in degrees C\n(e.g. 23.5)\n");
            return;
        }
        raw = sensors_read_temp_uncalibrated();
        offset = ref - raw;
        sensors_format_temp(r, sizeof(r), raw);
        sensors_format_temp(ref_t, sizeof(ref_t), ref);
        if (offset < -CONFIG_TEMP_OFFSET_MAX || offset > CONFIG_TEMP_OFFSET_MAX) {
            shell_printf(sh, "not changed: the sensor reads %s, and %s is more than %d degrees "
                             "from it.\nCheck the reference, and that the board has cooled to "
                             "room temperature.\n", r, ref_t, CONFIG_TEMP_OFFSET_MAX / 10);
            return;
        }
    }
    settings()->temp_offset = (int16_t)offset;
    save(sh);
    sensors_temp_offset_changed(old);
    format_temp_delta(d, sizeof(d), offset);
    if (!offset && !strcmp(value, "off")) {
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: chip temperature calibration off");
        shell_printf(sh, "chip temperature calibration: none (uncalibrated)\n");
        return;
    }
    log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: chip temperature calibration %s (sensor %s, "
                                           "reference %s)", d, r, ref_t);
    shell_printf(sh, "the sensor reads %s and the reference %s: %s is now added to every "
                     "reading\n", r, ref_t, d);
}

// `temp`: the chip's temperature now, and the lowest and highest since boot (core/sensors.h).
static void cmd_temp(shell_t *sh, char *args) {
    if (args) {
        if (!strncmp(args, "cal", 3) && (args[3] == ' ' || !args[3]))
            cmd_temp_cal(sh, args + 3);
        else
            shell_printf(sh, "usage: temp [cal [off | <degrees>]]\n");
        return;
    }
    char t[SENSORS_TEXT_MAX], d[SENSORS_TEXT_MAX + 1];
    sensors_format_temp(t, sizeof(t), sensors_read_temp());
    shell_printf(sh, "chip temperature: %s\n", t);
    print_since_boot(sh, sensors_temp_stats(), sensors_format_temp);
    shell_printf(sh, "This is the chip's temperature, not the room's (warmer than the air, more "
                     "under Wi-Fi load).\n");
    int offset = settings()->temp_offset;
    format_temp_delta(d, sizeof(d), offset);
    if (offset)
        shell_printf(sh, "Calibrated (`temp cal`): %s added to the sensor's reading.\n", d);
    else
        shell_printf(sh, "Uncalibrated: a few degrees off, good for change over time on this "
                         "board (`temp cal`).\n");
}

// `power`: the supply voltage (VSYS) now, the lowest and highest since boot, and USB power.
static void cmd_power(shell_t *sh) {
    int mv = sensors_read_vsys();
    if (mv == SENSORS_NONE) {
        shell_printf(sh, "this board has no supply voltage reading\n");
        return;
    }
    char v[SENSORS_TEXT_MAX];
    sensors_format_mv(v, sizeof(v), mv);
    int usb = sensors_usb_power();
    shell_printf(sh, "supply (VSYS): %s, %s\n", v,
                 usb > 0 ? "USB power present" : usb == 0 ? "no USB power" : "USB power unknown");
    print_since_boot(sh, sensors_vsys_stats(), sensors_format_mv);
    shell_printf(sh, "VSYS is the board's input: USB power through a diode (about 0.35 V below it), "
                     "or what feeds\nthe VSYS pin. Within a few percent; a reading taken during a "
                     "Wi-Fi transmission shows\nits dip (about 0.13 V on USB), so the lowest can "
                     "be a brief one.\n");
}

static void cmd_mem(shell_t *sh) {
    heap_info_t h = heap_info();
    shell_printf(sh, "heap:  %u in use, %u peak, %u total\n",
                 (unsigned)h.used, (unsigned)h.arena, (unsigned)h.total);
    // What each feature holds. "other" is the rest of what's in use: the SDK, the SD card and
    // Wi-Fi drivers, and malloc's own bookkeeping per block (the tags count usable bytes only).
    size_t tagged = 0;
    for (int t = 0; t < HEAP_TAGS; t++) {
        size_t used = heap_tag_used((heap_tag_t)t);
        tagged += used;
        shell_printf(sh, "  %-8s %7u, %u peak\n", heap_tag_name((heap_tag_t)t), (unsigned)used,
                     (unsigned)heap_tag_peak((heap_tag_t)t));
    }
    shell_printf(sh, "  %-8s %7u\n", "other", (unsigned)(h.used > tagged ? h.used - tagged : 0));
    cyw43_arch_lwip_begin();
    struct stats_mem net = lwip_stats.mem;
    cyw43_arch_lwip_end();
    // lwIP's heap only (MEM_SIZE); its packet and connection pools are in `wifi stats`.
    shell_printf(sh, "lwip:  %u in use, %u peak, %u total (lwIP's own heap, not part of the above; "
                     "its pools: wifi stats)\n",
                 (unsigned)net.used, (unsigned)net.max, (unsigned)net.avail);
    shell_printf(sh, "stack: %u peak of %u bytes\n",
                 (unsigned)stackmon_peak(), (unsigned)stackmon_size());
    if (lua_core1_stack_peak())
        shell_printf(sh, "lua:   %u peak of %u bytes of core 1 stack (last script)\n",
                     (unsigned)lua_core1_stack_peak(), (unsigned)LUA_CORE1_STACK);
#ifdef SOS_TEST_COMMANDS
    if (memhold_count())
        shell_printf(sh, "held:  %u in %d blocks by 'mem hold' (counted in other above)\n",
                     (unsigned)memhold_bytes(), memhold_count());
#endif
}

#ifdef SOS_TEST_COMMANDS
// `mem hold ...` and `mem release`, test builds only (core/memhold.h, issue #188): take heap
// memory and keep it, to make the out-of-memory paths happen.
static bool kb_arg(const char *s, size_t *bytes) {
    char *end;
    double kb = s ? strtod(s, &end) : -1;
    if (!s || end == s || *end || !(kb >= 0 && kb <= 1024))     // rejects NaN too
        return false;
    *bytes = (size_t)(kb * 1024);
    return true;
}

static void mem_held_summary(shell_t *sh) {
    shell_printf(sh, "held %u bytes in %d blocks; %u bytes of the heap unallocated, "
                     "largest block that fits: %u\n",
                 (unsigned)memhold_bytes(), memhold_count(), (unsigned)heap_free(),
                 (unsigned)memhold_largest());
}

static void cmd_mem_test(shell_t *sh, char *args) {
    if (!require_admin(sh))
        return;
    char *w1 = strtok(args, " "), *w2 = strtok(NULL, " "), *w3 = strtok(NULL, " ");
    char *w4 = strtok(NULL, " ");
    size_t n;
    if (!strcmp(w1, "release") && !w2) {
        size_t freed = memhold_release();
        shell_printf(sh, "released %u bytes; %u unallocated, largest block that fits: %u\n",
                     (unsigned)freed, (unsigned)heap_free(), (unsigned)memhold_largest());
        return;
    }
    if (!strcmp(w1, "release") && w2 && !strcmp(w2, "first") && !w4) {
        bool digits = !w3 || (strlen(w3) <= 4 && strspn(w3, "0123456789") == strlen(w3));
        int secs = digits && w3 ? atoi(w3) : 0;
        if (!digits || secs > 3600) {
            shell_printf(sh, "mem release first: 0 to 3600 seconds\n");
            return;
        }
        size_t first = memhold_release_first((unsigned)secs * 1000);
        if (!first)
            shell_printf(sh, "nothing held\n");
        else if (secs)
            shell_printf(sh, "the first block held (%u bytes) is freed in %d s\n", (unsigned)first,
                         secs);
        else
            mem_held_summary(sh);
        return;
    }
    if (!strcmp(w1, "hold") && w2 && !strcmp(w2, "leave") && kb_arg(w3, &n) && !w4) {
        memhold_leave(n);
        mem_held_summary(sh);
        return;
    }
    if (!strcmp(w1, "hold") && w2 && !strcmp(w2, "holes") && w3 && kb_arg(w4, &n) && n > 0
        && !strtok(NULL, " ")) {
        int want = atoi(w3);
        if (want < 1 || want > 32) {
            shell_printf(sh, "mem hold holes: 1 to 32 holes\n");
            return;
        }
        int made = memhold_holes(want, n);
        shell_printf(sh, "%d holes of %u bytes\n", made, (unsigned)n);
        mem_held_summary(sh);
        return;
    }
    if (!strcmp(w1, "hold") && kb_arg(w2, &n) && n > 0 && !w3) {
        if (!memhold_block(n))
            shell_printf(sh, "mem hold: no block of %u bytes to be had\n", (unsigned)n);
        mem_held_summary(sh);
        return;
    }
    shell_printf(sh, "usage: mem hold <KB> | mem hold leave <KB> | mem hold holes <count> <KB> "
                     "| mem release [first [<seconds>]]\n");
}
#endif

static void cmd_net(shell_t *sh) {
    int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    const char *s = link == CYW43_LINK_UP ? "up" : link == CYW43_LINK_JOIN ? "joined"
                  : link == CYW43_LINK_NOIP ? "no ip" : link == CYW43_LINK_FAIL ? "failed"
                  : link == CYW43_LINK_NONET ? "no network" : link == CYW43_LINK_BADAUTH ? "bad auth"
                  : "down";
    int32_t rssi = 0;
    cyw43_wifi_get_rssi(&cyw43_state, &rssi);

    char ip[16], gw[16];
    const char *host;
    cyw43_arch_lwip_begin();
    struct netif *n = &cyw43_state.netif[CYW43_ITF_STA];
    ip4addr_ntoa_r(netif_ip4_addr(n), ip, sizeof(ip));
    ip4addr_ntoa_r(netif_ip4_gw(n), gw, sizeof(gw));
    host = netif_get_hostname(n);
    cyw43_arch_lwip_end();

    shell_printf(sh, "wifi:  %s, rssi %ld dBm\n", s, (long)rssi);
    shell_printf(sh, "ip:    %s\n", ip);
    shell_printf(sh, "gw:    %s\n", gw);
    shell_printf(sh, "host:  %s\n", host ? host : "");
    unsigned arp_in, arp_out;
    wifi_arp_counts(&arp_in, &arp_out);
    shell_printf(sh, "arp:   %u received, %u sent; keepalive %s\n", arp_in, arp_out,
                 wifi_keepalive() ? "on" : "off (testing)");
}

// --- log -----------------------------------------------------------------------------------------

// `dmesg [-f <facility>]`: the RAM history, all of it or one facility's messages.
static void cmd_dmesg(shell_t *sh, char *args) {
    int only = -1;
    if (args) {
        char *opt = strtok(args, " ");
        char *name = strtok(NULL, " ");
        if (opt && name && !strcmp(opt, "-f") && !strtok(NULL, " "))
            only = log_facility_parse(name);
        if (only < 0) {
            shell_printf(sh, "usage: dmesg [-f none|kern|auth|daemon|user]\n");
            return;
        }
    }
    char line[LOG_LINE_MAX + 1];
    uint32_t cursor = 0;
    int priority;
    while (log_history_next(&cursor, line, sizeof(line), &priority))
        if (only < 0 || (priority & SOS_LOG_FACILITY_MASK) == only)
            shell_printf(sh, "%s\n", line);
}

// `logger [-p <level>] <message>`: a line in the log from any shell, like Unix logger. Always the
// user facility, tagged with the shell it came from, so a line can't pass for a system message or
// land in the auth log. Rate limited like scripts' output, for a loop piped into a session; each
// shell has its own limit, so one session's loop doesn't silence the others.
#define LOGGER_PER_SEC 10

static void cmd_logger(shell_t *sh, char *args) {
    int level = SOS_LOG_NOTICE;
    char *msg = args;
    if (msg && !strncmp(msg, "-p", 2) && (msg[2] == ' ' || !msg[2])) {
        char *name = msg + 2;
        while (*name == ' ')
            name++;
        char *after = strchr(name, ' ');
        if (after)
            *after++ = '\0';
        level = log_level_parse(name);
        msg = after;
        while (msg && *msg == ' ')
            msg++;
    }
    if (level < 0 || !msg || !*msg) {
        shell_printf(sh, "usage: logger [-p err|warning|notice|info|debug] <message>\n");
        return;
    }
    if (!log_quota_take(&sh->logger_quota, LOGGER_PER_SEC)) {
        shell_printf(sh, "logger: more than %d messages a second; not logged\n", LOGGER_PER_SEC);
        return;
    }
    unsigned refused = log_quota_dropped(&sh->logger_quota);
    if (refused)
        log_msg(SOS_LOG_USER | SOS_LOG_NOTICE,
                "logger: %u message%s refused (more than %d a second)", refused,
                refused == 1 ? "" : "s", LOGGER_PER_SEC);
    // "console", or "ssh <name>" (the session's key comment, as `who` shows it).
    const char *name = chat_name(sh);
    if (name && strcmp(name, sh->io->name))
        log_msg(SOS_LOG_USER | level, "logger (%s %s): %s", sh->io->name, name, msg);
    else
        log_msg(SOS_LOG_USER | level, "logger (%s): %s", sh->io->name, msg);
}

static void utc_text(char *buf, size_t len, int64_t unix_s) {
    time_t t = (time_t)unix_s;
    struct tm tm;
    gmtime_r(&t, &tm);
    // Fields bounded to their digits, so the compiler sees the text fits; same result.
    snprintf(buf, len, "%04u-%02u-%02u %02u:%02u UTC", (unsigned)(tm.tm_year + 1900) % 10000u,
             (unsigned)(tm.tm_mon + 1) % 100u, (unsigned)tm.tm_mday % 100u,
             (unsigned)tm.tm_hour % 100u, (unsigned)tm.tm_min % 100u);
}

static void show_rotation(shell_t *sh, const logfile_status_t *st) {
    shell_printf(sh, "rotate:  every %u day%s at %02u:%02u UTC (each file to <name>.1)\n",
                 st->rotate_days, st->rotate_days == 1 ? "" : "s", st->rotate_minute / 60,
                 st->rotate_minute % 60);
    if (st->rotated_known) {
        char last[32], next[32];
        utc_text(last, sizeof(last), st->rotated);
        utc_text(next, sizeof(next), st->next_rotation);
        shell_printf(sh, "         last %s, next %s\n", last, next);
    } else {
        shell_printf(sh, "         not counting yet (needs the card mounted and the clock set)\n");
    }
}

// `log rotate [every <days>d | at <HH:MM>]`: rotate the log files now, or set when they rotate
// (saved in the configuration). Console only: a rotation deletes the oldest files.
static void cmd_log_rotate(shell_t *sh, char *what, char *value) {
    if (!require_admin(sh))
        return;
    sos_config_t *cfg = settings();
    if (!what) {
        const char *err = logfile_rotate();
        if (err)
            shell_printf(sh, "not rotated: %s\n", err);
        else
            shell_printf(sh, "rotated: each file in %s is now <name>.1\n", LOGFILE_DIR);
        return;
    }
    char *end = NULL;
    if (!strcmp(what, "every") && value) {
        unsigned long days = strtoul(value, &end, 10);
        if (end == value || (*end && strcmp(end, "d")) || days < 1 ||
            days > CONFIG_LOG_ROTATE_DAYS_MAX) {
            shell_printf(sh, "not changed: give days from 1 to %u, e.g. 'log rotate every 7d'\n",
                         CONFIG_LOG_ROTATE_DAYS_MAX);
            return;
        }
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: log rotation every %lu day%s", days,
                days == 1 ? "" : "s");
        cfg->log_rotate_days = (uint16_t)days;
    } else if (!strcmp(what, "at") && value) {
        unsigned long h = strtoul(value, &end, 10), m = 0;
        bool ok = end != value && *end == ':' && end - value <= 2;
        if (ok) {
            char *mm = end + 1;
            m = strtoul(mm, &end, 10);
            ok = end - mm == 2 && !*end && h < 24 && m < 60;
        }
        if (!ok) {
            shell_printf(sh, "not changed: give a UTC time of day as HH:MM, e.g. 'log rotate at "
                             "03:30'\n");
            return;
        }
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: log rotation at %02lu:%02lu UTC", h, m);
        cfg->log_rotate_minute = (uint16_t)(h * 60 + m);
    } else {
        shell_printf(sh, "usage: log rotate [every <days>d | at <HH:MM>]\n");
        return;
    }
    save(sh);
    logfile_apply_settings();
    logfile_status_t st;
    logfile_status(&st);
    show_rotation(sh, &st);
}

static void cmd_log(shell_t *sh, char *args) {
    char *what = args ? strtok(args, " ") : NULL;
    char *level = what ? strtok(NULL, " ") : NULL;
    if (what && !strcmp(what, "rotate")) {
        cmd_log_rotate(sh, level, level ? strtok(NULL, " ") : NULL);
        return;
    }
    if (!what) {
        shell_printf(sh, "console: %s (printed on the serial console)\n",
                     log_level_name(log_console_level()));
        shell_printf(sh, "history: %s (kept for dmesg)\n", log_level_name(log_history_level()));
        shell_printf(sh, "file:    %s (written to %s on the SD card)\n",
                     log_level_name(logfile_level()), LOGFILE_DIR);
        shell_printf(sh, "levels:  err warning notice info debug\n");
        logfile_status_t st;
        logfile_status(&st);
        if (st.state == LOGFILE_STOPPED)
            shell_printf(sh, "files:   stopped: writing %s failed (%s); mount the card again "
                             "(e.g. `sd`) to start them\n", st.error_path, st.error);
        else
            shell_printf(sh, "files:   %s\n", st.state == LOGFILE_WRITING ? "writing"
                                               : st.state == LOGFILE_NO_CARD ? "no card mounted; "
                                                 "messages wait in the history"
                                               : "not started");
        shell_printf(sh, "written: %lu lines, %lu bytes this boot; %u bytes waiting\n",
                     (unsigned long)st.lines, (unsigned long)st.bytes, (unsigned)st.buffered);
        if (st.lost)
            shell_printf(sh, "lost:    %lu messages dropped from the history before they were "
                             "written\n", (unsigned long)st.lost);
        show_rotation(sh, &st);
        return;
    }
    int l = level ? log_level_parse(level) : -1;
    if (l < 0 || (strcmp(what, "console") && strcmp(what, "history") && strcmp(what, "file"))) {
        shell_printf(sh, "usage: log [console|history|file err|warning|notice|info|debug | "
                         "rotate [every <days>d | at <HH:MM>]]\n");
        return;
    }
    // The log is the record of logins and configuration changes: a session can read it
    // (dmesg) but not turn it down.
    if (!require_admin(sh))
        return;
    // Logged before it applies: `log history err` would otherwise drop its own record.
    log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: log %s level %s (until reboot)", what, level);
    if (!strcmp(what, "console"))
        log_set_console_level(l);
    else if (!strcmp(what, "history"))
        log_set_history_level(l);
    else
        logfile_set_level(l);
    shell_printf(sh, "%s level: %s (until reboot)\n", what, level);
}

// --- keys ----------------------------------------------------------------------------------------

static void print_key(shell_t *sh, unsigned index, const config_key_t *k) {
    char type[32], fp[64];
    sshd_fingerprint(k->blob, k->len, fp, sizeof(fp));
    shell_printf(sh, "%2u  %-20s %s  %s\n", index + 1, config_key_type(k, type, sizeof(type)), fp,
                 k->comment);
}

static void key_list(shell_t *sh) {
    const sos_config_t *c = settings();
    if (!c->key_count) {
        shell_printf(sh, "no trusted keys: SSH logins are not possible%s\n",
                     sh->io->admin ? " (add one with 'key add')" : "");
        return;
    }
    for (unsigned i = 0; i < c->key_count; i++)
        print_key(sh, i, &c->keys[i]);
}

// Parse and add one authorized_keys line. Returns true if the key was added.
static bool key_add_line(shell_t *sh, const char *line) {
    config_key_t key;
    const char *err = config_key_parse(line, &key);
    if (!err)
        err = config_key_add(settings(), &key);
    if (err) {
        shell_printf(sh, "not added: %s\n", err);
        return false;
    }
    save(sh);
    shell_printf(sh, "trusted:\n");
    print_key(sh, settings()->key_count - 1, &key);
    log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE,
            "config: trusted key added (%s)", key.comment[0] ? key.comment : "no comment");
    return true;
}

static void on_key_line(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (answer && *answer)
        key_add_line(sh, answer);
    else if (answer)
        shell_printf(sh, "nothing added\n");
}

static void cmd_key(shell_t *sh, char *args) {
    char *sub = args ? strtok(args, " ") : NULL;
    char *rest = sub ? strtok(NULL, "") : NULL;
    if (!sub || !strcmp(sub, "list")) {
        key_list(sh);
    } else if (!strcmp(sub, "add")) {
        if (!require_admin(sh))
            return;
        if (rest && *rest)
            key_add_line(sh, rest);
        else
            shell_ask(sh, "Paste the public key (e.g. ~/.ssh/id_ed25519.pub): ", false, on_key_line, NULL);
    } else if (!strcmp(sub, "del")) {
        if (!require_admin(sh))
            return;
        char *end;
        long n = rest ? strtol(rest, &end, 10) : 0;
        if (!rest || *end || n < 1 || !config_key_remove(settings(), (unsigned)(n - 1))) {
            shell_printf(sh, "usage: key del <n>, n as shown by 'key list'\n");
            return;
        }
        save(sh);
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: trusted key %ld removed", n);
        shell_printf(sh, "removed key %ld\n", n);
        if (!settings()->key_count)
            shell_printf(sh, "warning: no trusted keys left; SSH logins are no longer possible\n");
    } else {
        shell_printf(sh, "usage: key [list] | key add [line] | key del <n>\n");
    }
}

// --- Wi-Fi ---------------------------------------------------------------------------------------

static char pending_ssid[CONFIG_SSID_MAX + 1];

// The Wi-Fi settings and state, for `wifi` and `config`. "wifi password" says whether one is
// saved (never the password itself); the board has no login password, SSH is keys only.
static void wifi_show(shell_t *sh) {
    const sos_config_t *c = settings();
    unsigned n = config_wifi_count(c);
    const char *now = wifi_ssid();
    if (!n)
        shell_printf(sh, "wifi network:  (not configured)\n");
    for (unsigned i = 0; i < n; i++) {
        config_wifi_net_t net;
        config_wifi_get(c, i, &net);
        shell_printf(sh, "wifi network %u: %s, %s%s\n", i + 1, net.ssid,
                     net.password[0] ? "password set" : "open network",
                     now && !strcmp(now, net.ssid) ? " (connected)" : "");
        memset(&net, 0, sizeof(net));
    }
    shell_printf(sh, "wifi state:    %s\n", wifi_state());
    shell_printf(sh, "wifi power:    saving %s\n", c->wifi_powersave ? "on" : "off");
    shell_printf(sh, "wifi country:  %s\n", c->wifi_country[0] ? c->wifi_country : "worldwide");
}

static void stats_row(shell_t *sh, const char *name, const struct stats_proto *p) {
    shell_printf(sh, "  %-5s sent %-8u received %-8u dropped %u (checksum %u, length %u, memory %u, "
                     "protocol %u, other %u)\n", name, (unsigned)p->xmit, (unsigned)p->recv,
                 (unsigned)p->drop, (unsigned)p->chkerr, (unsigned)p->lenerr, (unsigned)p->memerr,
                 (unsigned)p->proterr, (unsigned)p->err);
}

// Radio state and packet counters, for chasing packet loss.
static void wifi_stats(shell_t *sh, bool chip) {
    wifi_radio_t r;
    if (wifi_radio(&r))
        shell_printf(sh, "radio:  signal %d dBm, channel %u, access point "
                         "%02x:%02x:%02x:%02x:%02x:%02x\n", (int)r.rssi, (unsigned)r.channel,
                     r.bssid[0], r.bssid[1], r.bssid[2], r.bssid[3], r.bssid[4], r.bssid[5]);
    else
        shell_printf(sh, "radio:  not connected\n");
    if (r.channel)
        shell_printf(sh, "        noise %d dBm, transmit rate %d.%d Mbit/s, transmit power limit %s"
                         "%d.%02d dBm\n", (int)r.noise, (int)(r.rate / 2), (int)(r.rate % 2) * 5,
                     r.txpower == 127 ? "not set: " : "", (int)(r.txpower / 4),
                     (int)(r.txpower % 4) * 25);
    struct stats_ st;
    cyw43_arch_lwip_begin();
    st = lwip_stats;
    struct stats_mem pool = *lwip_stats.memp[MEMP_PBUF_POOL], seg = *lwip_stats.memp[MEMP_TCP_SEG],
                     pcb = *lwip_stats.memp[MEMP_TCP_PCB];
    cyw43_arch_lwip_end();
    shell_printf(sh, "packets since boot:\n");
    stats_row(sh, "arp", &st.etharp);
    stats_row(sh, "ip", &st.ip);
    stats_row(sh, "icmp", &st.icmp);
    stats_row(sh, "udp", &st.udp);
    stats_row(sh, "tcp", &st.tcp);
    shell_printf(sh, "tcp:    %u segments resent, %u connections reset or failed\n",
                 (unsigned)st.mib2.tcpretranssegs,
                 (unsigned)(st.mib2.tcpattemptfails + st.mib2.tcpestabresets));
    shell_printf(sh, "pools:  packets %u/%u in use (most %u, %u failures); segments most %u/%u "
                     "(%u failures); connections most %u/%u (%u failures); heap most %u/%u "
                     "(%u failures)\n",
                 (unsigned)pool.used, (unsigned)pool.avail, (unsigned)pool.max, (unsigned)pool.err,
                 (unsigned)seg.max, (unsigned)seg.avail, (unsigned)seg.err, (unsigned)pcb.max,
                 (unsigned)pcb.avail, (unsigned)pcb.err, (unsigned)st.mem.max,
                 (unsigned)st.mem.avail, (unsigned)st.mem.err);
    // The driver's errors and the link's drops (docs/design/health.md §9.9: the `wifi` check).
    wifistat_t ws;
    wifistat_get(&ws);
    wifi_counts_t wc;
    wifi_counts(&wc);
    char fw[16];
    countcheck_fw_version(wc.ver, fw, sizeof(fw));
    shell_printf(sh, "driver: %u timeouts, %u bus, %u io, %u tx, %u other since boot; %u link "
                     "drops; firmware %s\n", (unsigned)ws.timeout, (unsigned)ws.bus,
                 (unsigned)ws.io, (unsigned)ws.tx, (unsigned)ws.other, (unsigned)wc.drops,
                 fw[0] ? fw : "unknown");
    if (!chip)
        return;
    // The chip's counters come as a version/length word and 32-bit values. Their layout differs
    // between firmware versions and isn't published for this one, so they're shown raw; compare
    // two readings. Positions in version 10, worked out from how they move (not confirmed):
    // 0 frames sent, 2 resent, 15 received, 45 beacon times, 49 RTS sent, 75 CTS received,
    // 85 our access point's beacons received, 87 other networks' beacons, 88 response timeouts,
    // 67/68 received with a bad checksum / header, 70 receptions started.
    // The buffer the chip answers into (1 KB), for this command only. The first 128 words are
    // shown, as before.
    uint32_t *c = heap_alloc(HEAP_NET, WIFI_CHIP_COUNTER_WORDS * sizeof(uint32_t));
    unsigned n = c ? wifi_chip_counters(c) : 0;
    if (n > 128)
        n = 128;
    if (!n) {
        shell_printf(sh, "chip counters: %s\n", c ? "not available" : "out of memory");
        heap_release(HEAP_NET, c);
        return;
    }
    shell_printf(sh, "chip counters (version %u, %u bytes), index: value, zeros omitted:\n",
                 (unsigned)(c[0] & 0xffff), (unsigned)(c[0] >> 16));
    for (unsigned i = 1, col = 0; i < n; i++)
        if (c[i])
            shell_printf(sh, "%3u: %-10u%s", i - 1, (unsigned)c[i], ++col % 5 ? " " : "\n");
    shell_printf(sh, "\n");
    heap_release(HEAP_NET, c);
}

// `wifi set` (arg NULL) replaces the first network; `wifi add` (arg non-NULL) adds one after
// the others.
static void on_wifi_password(shell_t *sh, char *answer, void *arg) {
    if (!answer)
        return;
    sos_config_t *c = settings();
    bool add = arg != NULL;
    const char *err = add ? config_wifi_add(c, pending_ssid, answer)
                          : config_wifi_set_first(c, pending_ssid, answer);
    memset(answer, 0, strlen(answer));
    if (err) {
        shell_printf(sh, "not changed: %s\n", err);
        return;
    }
    save(sh);
    if (add) {
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: Wi-Fi network '%s' added", pending_ssid);
        if (!wifi_up())
            wifi_reconnect();       // it may be the one in reach
    } else {
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: Wi-Fi network set to '%s'", pending_ssid);
        wifi_reconnect();
    }
}

// Tab at an SSID question: the networks a scan hears (net/wifi.c), strongest first, each with
// its signal in the list.
typedef struct {
    shell_value_fn fn;
    void *arg;
} ssid_tab_t;

static void ssid_one(const char *ssid, int rssi, void *arg) {
    ssid_tab_t *t = arg;
    char note[16];
    snprintf(note, sizeof(note), "%d dBm", rssi);
    t->fn(ssid, note, t->arg);
}

static bool tab_ssids(shell_t *sh, shell_value_fn fn, void *arg) {
    (void)sh;
    ssid_tab_t t = {fn, arg};
    return wifi_names_each(ssid_one, &t);
}

// Ask for an SSID, with Tab offering the networks around: a scan starts now (about 0.5 s). The
// answer's callback calls wifi_names_stop().
static void ask_ssid(shell_t *sh, const char *prompt, shell_answer_fn fn, void *arg) {
    shell_ask(sh, prompt, false, fn, arg);
    wifi_names_start();
    shell_ask_complete(sh, tab_ssids);
}

static void on_wifi_ssid(shell_t *sh, char *answer, void *arg) {
    wifi_names_stop();
    if (!answer)
        return;
    const char *err = config_check_ssid(answer);
    if (!err && arg && config_wifi_find(settings(), answer) >= 0)
        err = "that network is already saved (remove it first to change its password)";
    if (!err && arg && config_wifi_count(settings()) == CONFIG_WIFI_NETS)
        err = "the network list is full (5 networks)";
    if (err) {
        shell_printf(sh, "not changed: %s\n", err);
        return;
    }
    strcpy(pending_ssid, answer);
    shell_ask(sh, "Password (empty for an open network): ", true, on_wifi_password, arg);
}

// `wifi remove <n>` and `wifi prefer <n>`: n as `wifi` numbers them. Returns the index, or -1
// after saying why not.
static int wifi_number(shell_t *sh, const char *arg) {
    char *end;
    long n = strtol(arg, &end, 10);
    if (*arg == '\0' || *end != '\0' || n < 1 || n > (long)config_wifi_count(settings())) {
        shell_printf(sh, "no network %s ('wifi' lists them)\n", arg);
        return -1;
    }
    return (int)n - 1;
}

static void cmd_wifi(shell_t *sh, char *args) {
    if (!args || !*args) {
        wifi_show(sh);
    } else if (!strcmp(args, "set")) {
        if (require_admin(sh))
            ask_ssid(sh, "SSID: ", on_wifi_ssid, NULL);
    } else if (!strcmp(args, "add")) {
        if (require_admin(sh))
            ask_ssid(sh, "SSID: ", on_wifi_ssid, (void *)1);
    } else if (!strncmp(args, "remove ", 7)) {
        int i;
        if (!require_admin(sh) || (i = wifi_number(sh, args + 7)) < 0)
            return;
        config_wifi_net_t net;
        config_wifi_get(settings(), (unsigned)i, &net);
        const char *now = wifi_ssid();
        bool was_connected = now && !strcmp(now, net.ssid);
        config_wifi_remove(settings(), (unsigned)i);
        save(sh);
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: Wi-Fi network '%s' removed", net.ssid);
        memset(&net, 0, sizeof(net));
        if (!config_wifi_count(settings()))
            shell_printf(sh, "warning: no Wi-Fi network left\n");
        if (was_connected)
            wifi_reconnect();
    } else if (!strncmp(args, "prefer ", 7)) {
        int i;
        if (!require_admin(sh) || (i = wifi_number(sh, args + 7)) < 0)
            return;
        config_wifi_prefer(settings(), (unsigned)i);
        save(sh);
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE,
                "config: Wi-Fi network '%s' preferred", settings()->wifi_ssid);
        if (wifi_up() && strcmp(wifi_ssid(), settings()->wifi_ssid))
            shell_printf(sh, "takes effect on the next Wi-Fi connect ('wifi connect')\n");
    } else if (!strcmp(args, "connect")) {
        if (require_admin(sh))
            wifi_reconnect();
    } else if (!strcmp(args, "keepalive on") || !strcmp(args, "keepalive off")) {
        // For testing only, so not saved: it's back on after a reboot.
        if (!require_admin(sh))
            return;
        wifi_set_keepalive(!strcmp(args, "keepalive on"));
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE,
                "wifi: keepalive %s (until reboot)", wifi_keepalive() ? "on" : "off");
    } else if (!strcmp(args, "stats") || !strcmp(args, "stats chip")) {
        wifi_stats(sh, args[5] != 0);
#ifdef SOS_TEST_COMMANDS
    } else if (!strcmp(args, "fault drop")) {
        // Test builds (docs/design/health.md §9.9): a link drop nobody asked for, as the `wifi`
        // check counts one; wifi.c sees the link go and reconnects.
        if (!require_admin(sh))
            return;
        if (wifi_fault_drop()) {
            log_msg(SOS_LOG_DAEMON | SOS_LOG_NOTICE, "wifi: fault: link dropped (test command)");
            shell_printf(sh, "dropped: wifi reconnects as after a real drop\n");
        } else {
            shell_printf(sh, "not connected\n");
        }
#endif
    } else if (!strncmp(args, "iovar ", 6)) {
        // Debugging only: `wifi iovar <name> [value]` reads or writes a chip variable.
        if (!require_admin(sh))
            return;
        char *name = args + 6, *val = strchr(name, ' ');
        if (val)
            *val++ = '\0';
        uint32_t v = val ? (uint32_t)strtoul(val, NULL, 0) : 0;
        int r = val ? wifi_iovar_set(name, v) : wifi_iovar_get(name, &v);
        shell_printf(sh, "%s %s: result %d, value %u (0x%x)\n", val ? "set" : "get", name, r,
                     (unsigned)v, (unsigned)v);
    } else if (!strncmp(args, "country ", 8)) {
        if (!require_admin(sh))
            return;
        const char *v = !strcmp(args + 8, "worldwide") ? "" : args + 8;
        const char *err = config_check_country(v);
        if (err) {
            shell_printf(sh, "not changed: %s\n", err);
            return;
        }
        snprintf(settings()->wifi_country, sizeof(settings()->wifi_country), "%s", v);
        save(sh);
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: wifi country %s", v[0] ? v : "worldwide");
        shell_printf(sh, "takes effect at the next boot ('reboot')\n");
    } else if (!strcmp(args, "power")) {
        shell_printf(sh, "power saving: %s\n", settings()->wifi_powersave ? "on" : "off");
    } else if (!strcmp(args, "power on") || !strcmp(args, "power off")) {
        if (!require_admin(sh))
            return;
        settings()->wifi_powersave = !strcmp(args, "power on");
        save(sh);
        wifi_apply_power();
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE,
                "config: wifi power saving %s", settings()->wifi_powersave ? "on" : "off");
    } else {
        shell_printf(sh, "usage: wifi [set|add|remove <n>|prefer <n>|connect|stats [chip]|power [on|off]|\n            country <XX>|worldwide|keepalive on|off|iovar <name> [<value>]]\n");
    }
}

// --- hostname, user, config ----------------------------------------------------------------------

static void set_field(shell_t *sh, const char *what, char *value, char *field, size_t size,
                      const char *(*check)(const char *)) {
    if (!value || !*value) {
        shell_printf(sh, "%s: %s\n", what, field);
        return;
    }
    if (!require_admin(sh))
        return;
    const char *err = check(value);
    if (err) {
        shell_printf(sh, "not changed: %s\n", err);
        return;
    }
    snprintf(field, size, "%s", value);
    save(sh);
    log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: %s set to '%s'", what, field);
    if (field == settings()->hostname)
        shell_printf(sh, "takes effect on the next Wi-Fi connect ('wifi connect')\n");
}

static void on_reset_confirm(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (!answer || strcmp(answer, "yes")) {
        shell_printf(sh, "not reset\n");
        return;
    }
    int old_temp_offset = settings()->temp_offset;
    bool erased = settings_reset();
    // The live configuration is gone either way, the owner secret with it: an encrypted card
    // doesn't stay open under it, and the SSH host key was that owner's. SSH stops now (it
    // would otherwise go on answering with the old key until a reboot) and starts again, with
    // a new host key, once the board is set up.
    sdcard_owner_gone();
    sshd_stop();
    hostkey_owner_gone();
    if (erased)
        shell_printf(sh, "configuration erased: no Wi-Fi network and no trusted keys; run "
                         "'setup'\n");
    else
        shell_printf(sh, "error: erasing the saved configuration failed (see dmesg). It's "
                         "cleared in memory, but\nthe saved one is still in flash and comes "
                         "back at the next boot.\n");
    wifi_apply_power();
    timesync_apply_cal();               // the RTC goes back to uncalibrated now, not at the next boot
    sensors_temp_offset_changed(old_temp_offset);   // and the chip temperature
    wifi_reconnect();
}

// --- glitch detector -----------------------------------------------------------------------------

static void glitch_status(shell_t *sh) {
    unsigned setting = config_glitch_mode(settings()->glitch_detector);
    bool is_default = settings()->glitch_detector == CONFIG_GLITCH_DETECTOR_DEFAULT;
    int armed = glitchdet_armed_sensitivity();
    if (setting == 0)
        shell_printf(sh, "glitch detector: off\n");
    else if (setting >= 5) {
        shell_printf(sh, "glitch detector: watching at sensitivity %u (0 lowest, 3 highest), not "
                         "armed: a trigger is logged, the board isn't reset\n", (setting - 1) & 3u);
        if (glitchdet_watch_triggers()) {
            char both[SENSORS_BOTH_MAX];
            sensors_format_both(both, sizeof(both), glitchdet_watch_first_temp(),
                                glitchdet_watch_first_vsys());
            shell_printf(sh, "TRIGGERED since boot: detectors 0x%x, first after %lu s up (%s). "
                             "Armed, it would have reset the board\n",
                         (unsigned)glitchdet_watch_triggers(),
                         (unsigned long)glitchdet_watch_first_s(), both);
        } else
            shell_printf(sh, "no trigger since boot\n");
    } else
        shell_printf(sh, "glitch detector: set to sensitivity %u (0 lowest, 3 highest)%s, %s\n",
                     setting - 1, is_default ? ", the default" : "",
                     armed >= 0 ? "armed" : glitchdet_guard_tripped()
                         ? "NOT armed at this boot: it reset the board several times in a row "
                           "right after starting"
                         : "not armed");
    shell_printf(sh, "set up by software after boot: it shows whether the detector resets a "
                     "healthy board; it is not protection\n");
    shell_printf(sh, "detector resets since power-on: %lu\n", (unsigned long)glitchdet_resets());
    if (glitchdet_caused_reset()) {
        shell_printf(sh, "the last reset was the detector's: detectors 0x%x fired, after %lu s up\n",
                     (unsigned)glitchdet_last_triggers(),
                     (unsigned long)glitchdet_last_uptime_s());
        char both[SENSORS_BOTH_MAX];
        sensors_format_both(both, sizeof(both), glitchdet_last_temp(), glitchdet_last_vsys());
        if (glitchdet_last_temp() != SENSORS_NONE || glitchdet_last_vsys() != SENSORS_NONE)
            shell_printf(sh, "before that reset: %s (last readings, taken every %u s)\n", both,
                         SENSORS_SAMPLE_S);
        else
            shell_printf(sh, "no chip temperature or supply reading was kept from before that "
                             "reset\n");
    }
}

// `glitch`: the detector's setting, state and resets. Console only: `glitch on <0-3>` arms it now
// and at every boot; `glitch off` disarms it; `glitch default` goes back to the default (armed at
// the highest sensitivity); `glitch trig` makes a detector fire, to see a detector reset reported.
static void cmd_glitch(shell_t *sh, char *args) {
    char *sub = args ? strtok(args, " ") : NULL;
    char *arg = sub ? strtok(NULL, " ") : NULL;
    if (!sub) {
        glitch_status(sh);
        return;
    }
    if (!require_admin(sh))
        return;
    if (!strcmp(sub, "off") && !arg) {
        settings()->glitch_detector = CONFIG_GLITCH_DETECTOR_OFF;
        glitchdet_apply(0);
        save(sh);
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: glitch detector off");
        shell_printf(sh, "glitch detector off, now and at boot\n");
    } else if (!strcmp(sub, "default") && !arg) {
        settings()->glitch_detector = CONFIG_GLITCH_DETECTOR_DEFAULT;
        save(sh);
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: glitch detector back to the default");
        if (glitchdet_apply(CONFIG_GLITCH_DEFAULT_MODE))
            shell_printf(sh, "glitch detector back to the default: armed at sensitivity %u, now "
                             "and at boot\n", (unsigned)CONFIG_GLITCH_DEFAULT_MODE - 1);
        else
            shell_printf(sh, "saved, but arming it didn't take (see dmesg)\n");
    } else if (!strcmp(sub, "on") && arg && arg[0] >= '0' && arg[0] <= '3' && !arg[1]) {
        unsigned setting = (unsigned)(arg[0] - '0') + 1;
        // Saved first: if arming resets the board at once, the guard at boot deals with it.
        settings()->glitch_detector = (uint8_t)setting;
        save(sh);
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE,
                "config: glitch detector on, sensitivity %c", arg[0]);
        if (glitchdet_apply(setting))
            shell_printf(sh, "glitch detector armed at sensitivity %c, now and at boot.\n"
                             "If it resets the board, `glitch off` turns it off; after %u resets "
                             "in a row right\nafter starting, a boot leaves it unarmed so that "
                             "you can.\n", arg[0], GLITCHDET_GUARD_RESETS);
        else
            shell_printf(sh, "saved, but arming it didn't take (see dmesg)\n");
    } else if (!strcmp(sub, "watch") && arg && arg[0] >= '0' && arg[0] <= '3' && !arg[1]) {
        unsigned setting = (unsigned)(arg[0] - '0') + 5;
        settings()->glitch_detector = (uint8_t)setting;
        save(sh);
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE,
                "config: glitch detector watching, sensitivity %c", arg[0]);
        if (glitchdet_apply(setting))
            shell_printf(sh, "watching at sensitivity %c, now and at boot: a trigger is logged "
                             "and shown by `glitch`; the board isn't reset\n", arg[0]);
        else
            shell_printf(sh, "saved, but setting the sensitivity didn't take (see dmesg)\n");
    } else if (!strcmp(sub, "trig") && !arg) {
        if (glitchdet_armed_sensitivity() < 0 && glitchdet_watching_sensitivity() < 0) {
            shell_printf(sh, "the detector is off, so nothing would be seen\n");
            return;
        }
        if (glitchdet_armed_sensitivity() >= 0) {
            shell_printf(sh, "simulating a detector trigger: the board resets now\n");
            sleep_ms(50);
        } else {
            shell_printf(sh, "simulating a detector trigger: it should be logged within a "
                             "second\n");
        }
        glitchdet_simulate();
    } else {
        shell_printf(sh, "usage: glitch [watch <0-3> | on <0-3> | off | default | trig]\n");
    }
}

// --- root secret ---------------------------------------------------------------------------------

static void on_rootsecret_forget(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (!answer || strcmp(answer, "yes")) {
        shell_printf(sh, "the record is kept\n");
        return;
    }
    shell_printf(sh, rootflash_forget() ? "the record is cleared; `reboot` makes a new root secret\n"
                                        : "clearing the record failed (see dmesg)\n");
}

#ifdef SOS_BURN_IMAGE
// --- `rootsecret burn` (the burn image only) ---------------------------------------------------------
//
// Writes this board's root secret into OTP, for good (core/rootburn.h). On the console only,
// and only after: the board's serial typed in, a word typed in, and the BOOTSEL button held.
// `rootsecret burn plan` says what it would do and writes nothing.

#include "core/rootburn_board.h"

#define BURN_WAIT_MS 30000u             // how long it waits for the button
#define BURN_HOLD_MS 1000u              // how long the button must be held before it starts

static bool burn_rehearse;
static bool burn_stopped;               // a run ended in a way that must be looked into first
static uint64_t burn_deadline_ms, burn_held_since_ms;

static void burn_report(shell_t *sh, const rootburn_report_t *rep) {
    shell_printf(sh, "\n%s.\n", rootburn_result_text(rep->result));
    if (rep->nothing_landed)
        shell_printf(sh, "Nothing landed in OTP: page %u still reads blank and was left "
                         "unlocked.\n", rep->page);
    else if (rep->wrote)
        shell_printf(sh, "OTP page %u: %s\n", rep->page,
                     rep->page_locked ? "locked against the bootloader" : "NOT LOCKED");
    if (rep->result == ROOTBURN_WRITE && rep->row)
        shell_printf(sh, "It stopped at row 0x%03x.\n", rep->row);
    log_msg(SOS_LOG_KERN | (rootburn_must_stop(rep->result) ? SOS_LOG_ERR : SOS_LOG_NOTICE),
            "rootsecret burn: %s (OTP page %u, row 0x%03x, wrote %s, page %s%s)",
            rootburn_result_text(rep->result), rep->page, rep->row, rep->wrote ? "yes" : "no",
            rep->page_locked ? "locked" : "not locked",
            rep->nothing_landed ? ", nothing landed" : "");
    if (rootburn_must_stop(rep->result)) {
        burn_stopped = true;
        shell_printf(sh, "\nSTOP HERE. Do not run `rootsecret burn` again, on this board or "
                         "another, until the cause is\nunderstood: a fault in how OTP is "
                         "written would spend the remaining pages the same way.\nThis image "
                         "refuses further runs until the board is restarted. `dmesg` has the "
                         "details.\n");
    } else if (rep->result == ROOTBURN_OK) {
        shell_printf(sh, "Now flash the everyday firmware; `rootsecret` there must report the "
                         "secret in OTP page %u.\n", rep->page);
    } else if (rep->result == ROOTBURN_REHEARSED) {
        shell_printf(sh, "That is how a rehearsal should end. Run `rootsecret burn` now, in "
                         "this sitting, to write the\nreal secret into the next page.\n");
    }
}

static bool burn_job(shell_t *sh, void *arg, bool cancel) {
    (void)arg;
    uint64_t now = to_ms_since_boot(get_absolute_time());
    if (cancel) {
        shell_printf(sh, "\ncancelled: nothing was written\n");
        return false;
    }
    if (!rootburn_button()) {
        burn_held_since_ms = 0;
        if (now >= burn_deadline_ms) {
            shell_printf(sh, "\nthe button wasn't held in time: nothing was written\n");
            return false;
        }
        return true;
    }
    if (!burn_held_since_ms)
        burn_held_since_ms = now;
    if (now - burn_held_since_ms < BURN_HOLD_MS)
        return true;
    // Nothing may reset the board in the middle. The watchdog is fed between writes
    // (rootburn_board.c). The glitch detector, armed by software, is switched off for the
    // length of the burn: whether programming OTP disturbs the supply enough to trip it isn't
    // known, and a detector reset would spend the page. It protects nothing (core/glitchdet.h).
    shell_printf(sh, "\nwriting OTP: keep the button held and don't unplug the board...\n"
                     "(the glitch detector is off until this is done)\n");
    glitchdet_apply(0);
    rootburn_report_t rep;
    uint64_t started_us = time_us_64();
    rootburn_board_run(burn_rehearse, &rep);
    uint32_t took_ms = (uint32_t)((time_us_64() - started_us) / 1000);
    glitchdet_apply(config_glitch_mode(settings()->glitch_detector));
    burn_report(sh, &rep);
    // How long OTP programming takes on this chip isn't known until it has been done once:
    // worth writing down (dev-1's was not: docs/otp/dev-1/RECORD.md, #100).
    shell_printf(sh, "It took %lu ms.\n", (unsigned long)took_ms);
    log_msg(SOS_LOG_KERN | SOS_LOG_NOTICE, "rootsecret burn: took %lu ms", (unsigned long)took_ms);
    shell_printf(sh, "You can let go of the button.\n");
    return false;
}

static void on_burn_word(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (!answer || strcmp(answer, burn_rehearse ? "REHEARSE" : "BURN")) {
        shell_printf(sh, "not confirmed: nothing was written\n");
        return;
    }
    burn_deadline_ms = to_ms_since_boot(get_absolute_time()) + BURN_WAIT_MS;
    burn_held_since_ms = 0;
    shell_printf(sh, "Hold the BOOTSEL button now, and keep it held until told to let go "
                     "(%u s to start; Ctrl-C cancels).\nIf the board restarts while the button "
                     "is held it comes up in the USB bootloader, not broken:\n`picotool reboot` "
                     "brings it back.\n", BURN_WAIT_MS / 1000u);
    shell_run_job(sh, burn_job, NULL);
}

static void on_burn_serial(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    pico_get_unique_board_id_string(serial, sizeof(serial));
    if (!answer || strcasecmp(answer, serial)) {
        shell_printf(sh, "that isn't this board's serial: nothing was written\n");
        return;
    }
    shell_ask(sh, burn_rehearse ? "Type REHEARSE to spend one OTP page on purpose: "
                                : "Type BURN to write to this board's OTP: ",
              false, on_burn_word, NULL);
}

static void cmd_rootsecret_burn(shell_t *sh, const char *what) {
    while (*what == ' ')
        what++;
    bool plan = !strcmp(what, "plan");
    if (*what && !plan && strcmp(what, "rehearse")) {
        shell_printf(sh, "usage: rootsecret burn [plan | rehearse]\n");
        return;
    }
    rootburn_report_t rep;
    unsigned to_lock;
    rootburn_plan(&rep, &to_lock);
    if (rep.result == ROOTBURN_OK) {
        shell_printf(sh, "It would use OTP page %u (rows 0x%03x to 0x%03x)", rep.page,
                     rep.page * 64, rep.page * 64 + 63);
        if (to_lock)
            shell_printf(sh, ", after locking %u earlier, spent page%s", to_lock,
                         to_lock == 1 ? "" : "s");
        shell_printf(sh, ".\n");
    } else if (rep.result == ROOTBURN_RELOCKED) {
        shell_printf(sh, "This board has its root secret in OTP page %u. It would write no "
                         "secret; it would lock %u page%s\nthat the bootloader can still "
                         "write.\n", rep.page, to_lock, to_lock == 1 ? "" : "s");
    } else {
        shell_printf(sh, "%s.\n", rootburn_result_text(rep.result));
        return;
    }
    if (plan) {
        shell_printf(sh, "BOOTSEL button: %s. Nothing was written.%s\n",
                     rootburn_button() ? "held" : "not held",
                     burn_stopped ? " (This image refuses further runs until a restart: an "
                                    "earlier run failed.)" : "");
        return;
    }
    if (!require_admin(sh))
        return;
    if (burn_stopped) {
        shell_printf(sh, "An earlier run on this board ended in an error. Nothing more is "
                         "written until that is understood\nand the board has been "
                         "restarted (`dmesg` has what happened).\n");
        return;
    }
    burn_rehearse = !strcmp(what, "rehearse");
    if (burn_rehearse && rep.result == ROOTBURN_RELOCKED) {
        shell_printf(sh, "This board already has its secret: there is nothing to rehearse.\n");
        return;
    }
    shell_printf(sh, "\nTHIS WRITES ONE-TIME-PROGRAMMABLE MEMORY. IT CANNOT BE UNDONE.\n");
    if (rep.result == ROOTBURN_RELOCKED)
        shell_printf(sh, "It locks the pages named above, for good. No secret is written.\n");
    else if (burn_rehearse)
        shell_printf(sh, "Rehearsal: it writes 32 random bytes into that page and stops before "
                         "they become a root\nsecret. The page is then spent for good, and "
                         "locked. This board has %u such pages.\n", (unsigned)ROOTOTP_PAGES);
    else
        shell_printf(sh, "It writes a new root secret into that page and locks it. Everything "
                         "this board has\nsealed so far (configuration, host key, cards) is "
                         "under its old root secret and will\nnot open afterwards. The secret "
                         "is made on the board and never shown.\n");
    shell_ask(sh, "Type this board's serial number to go on: ", false, on_burn_serial, NULL);
}
#endif // SOS_BURN_IMAGE

// `rootsecret`: the state of the secret every stored key is derived from (core/keyprov.h);
// `rootsecret forget` (console): clear a record that can't be used, the explicit way out. A
// usable secret can't be cleared here: that would silently lock everything sealed with it.
static void cmd_rootsecret(shell_t *sh, const char *args) {
    int state = rootflash_state();
#ifdef SOS_BURN_IMAGE
    if (args && !strncmp(args, "burn", 4) && (args[4] == '\0' || args[4] == ' ')) {
        cmd_rootsecret_burn(sh, args + 4);
        return;
    }
#endif
    if (rootsource() != ROOTSRC_FLASH) {
        // In OTP, or none at all: there is no flash record in play, and nothing to forget.
        shell_printf(sh, "root secret: %s\n", rootsource_describe());
        if (rootsource() == ROOTSRC_OTP)
            shell_printf(sh, "its OTP page is %s\n", rootsource_otp_locked()
                             ? "locked until the next reset"
                             : "still readable by the firmware (boot hasn't finished)");
        if (args && *args)
            shell_printf(sh, "(`rootsecret forget` is for a flash record; this board doesn't "
                             "use one)\n");
        return;
    }
    if (!args || !*args) {
        if (!rootflash_usable())
            shell_printf(sh, "root secret: none (the firmware image overlaps its flash sector)\n");
        else if (state == ROOTSECRET_OK)
            shell_printf(sh, "root secret: present, in flash (open board: it isn't protected "
                             "from someone who has the board)%s\n",
                         rootflash_degraded() ? "\none of its two copies is damaged; the other "
                                                "is in use" : "");
        else if (state == ROOTSECRET_NONE)
            shell_printf(sh, "root secret: none (making it at boot failed, see dmesg; `reboot` "
                             "tries again)\n");
        else
            shell_printf(sh, "root secret: present but unusable (damaged, or written by newer "
                             "firmware). It's kept as it is:\nnewer firmware may read it. "
                             "`rootsecret forget` clears it; what was stored under it is then "
                             "lost.\n");
        return;
    }
    if (strcmp(args, "forget")) {
        shell_printf(sh, "usage: rootsecret [forget]\n");
        return;
    }
    if (!sh->io->admin) {
        shell_printf(sh, "the root secret can only be cleared on the serial console\n");
        return;
    }
    if (consoleonly()) {
        shell_printf(sh, "this is a console-only board: its root secret record isn't cleared "
                         "from here\n");
        return;
    }
    if (state != ROOTSECRET_INVALID || !rootflash_usable()) {
        shell_printf(sh, state == ROOTSECRET_OK ? "the root secret is usable; it isn't cleared\n"
                                                : "there's no record to clear\n");
        return;
    }
    shell_printf(sh, "This erases the root secret's whole flash sector. Whatever was stored under "
                     "this root\nsecret can't be read again, by any firmware.\n");
    shell_ask(sh, "Clear the unusable record? Type 'yes': ", false, on_rootsecret_forget, NULL);
}

static void cmd_config(shell_t *sh, char *args) {
    if (args && !strcmp(args, "reset")) {
        if (require_admin(sh)) {
            shell_printf(sh, "This also destroys the owner secret that unlocks this board's "
                             "encrypted SD cards.\nCards it encrypted can then only be read on "
                             "a PC, with the key shown when each was\nformatted. Setting the "
                             "board up again does not bring them back.\n");
            shell_ask(sh, "Erase Wi-Fi settings, trusted keys and the owner secret? Type 'yes': ",
                      false, on_reset_confirm, NULL);
        }
        return;
    }
    if (args && *args) {
        shell_printf(sh, "usage: config [reset]\n");
        return;
    }
    const sos_config_t *c = settings();
    shell_printf(sh, "hostname:      %s\n", c->hostname);
    shell_printf(sh, "console:       %s\n", settings_console_has_password()
                                              ? "password set" : "no password (`passwd` sets one)");
    wifi_show(sh);
    shell_printf(sh, "keys:          %u trusted ('key list')\n", c->key_count);
    shell_printf(sh, "nts:           %s%s, ", c->ntp_server[0] ? c->ntp_server : SOS_NTS_SERVER,
                 c->ntp_server[0] ? "" : " (build default)");
    if (c->ntp_sync_hours)
        shell_printf(sh, "sync every %u h\n", (unsigned)c->ntp_sync_hours);
    else
        shell_printf(sh, "periodic sync off\n");
    char cal[40];
    timesync_cal_str(cal, sizeof(cal));
    shell_printf(sh, "rtc cal:       %s\n", cal);
    shell_printf(sh, "time zone:     %s\n", c->tz_name[0] ? c->tz_name : "UTC (none set)");
    char tcal[SENSORS_TEXT_MAX + 1];
    format_temp_delta(tcal, sizeof(tcal), c->temp_offset);
    shell_printf(sh, "temp cal:      %s\n", c->temp_offset ? tcal : "off");
    unsigned days = c->log_rotate_days ? c->log_rotate_days : CONFIG_LOG_ROTATE_DAYS_DEFAULT;
    shell_printf(sh, "log rotation:  every %u day%s at %02u:%02u UTC%s\n", days,
                 days == 1 ? "" : "s", c->log_rotate_minute / 60u, c->log_rotate_minute % 60u,
                 c->log_rotate_days || c->log_rotate_minute ? "" : " (default)");
    if (!settings_persistent())
        shell_printf(sh, "warning:       flash storage unavailable; changes aren't saved\n");
}

// --- the console's lock (shell/login.h) ------------------------------------------------------------

static void cmd_setup(shell_t *sh);
static void ask_new_password(shell_t *sh, void (*then)(shell_t *sh));

static uint64_t console_now_ms(void) { return to_ms_since_boot(get_absolute_time()); }
static int console_check_step(void) {
    int r = settings_console_check_step();
    return r == 0 ? LOGIN_MORE : r == 1 ? LOGIN_RIGHT : LOGIN_WRONG;
}

// With no usable root secret nothing sealed can be read, the password included. The console
// must not be open because of that: it stays locked, with only the commands that deal with it.
static const char *console_no_keys(void) {
    if (rootsource_ok())
        return NULL;
    // A fresh open board whose root secret a failed health check refused (core/health.h): the
    // root-secret advice would send whoever is here the wrong way. Once the check passes, a
    // reboot makes the secret as usual. Only that refusal: a host key refused on a board whose
    // OTP secret is unusable is no reason to blame the check for the secret.
    const char *held = rootsource() == ROOTSRC_FLASH ? rootflash_refused_by() : NULL;
    if (held) {
        static char why[160];
        snprintf(why, sizeof(why), "key making refused: health check %s failed.\n"
                 "`health` shows the checks' results, `reboot` restarts.", held);
        return why;
    }
    // Only a flash record can be cleared and made again from here. A secret that should be in
    // OTP and isn't usable is not something a command puts right.
    return rootsource() == ROOTSRC_FLASH
               ? "this board's root secret can't be used, so nothing it stores can be read.\n"
                 "`rootsecret` says more, `rootsecret forget` starts over (what was stored is "
                 "lost), `reboot` restarts."
               : "this board has no root secret it can use, so nothing it stores can be read.\n"
                 "`rootsecret` says why, `reboot` restarts.";
}

static bool console_rescue_allowed(const char *cmd) {
    return !strcmp(cmd, "rootsecret") || !strcmp(cmd, "rootsecret forget") ||
           !strcmp(cmd, "reboot") || !strcmp(cmd, "health");    // results only: never run|full
}

static void console_opened(shell_t *sh) {
    log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "console: logged in");
    shell_greet(sh);
}

// No console password is set: a new board, a configuration from before console passwords, or
// one provisioned without a password. Whoever is at the console sets it now.
static void console_first_use(shell_t *sh) {
    shell_greet(sh);
    if (!settings()->wifi_ssid[0] && !settings()->key_count) {
        cmd_setup(sh);                  // a board that was never set up: the whole of setup
        return;
    }
    shell_printf(sh, "This board has no console password yet. It's asked for before the console "
                     "takes any\ncommand. Ctrl-C leaves the console without one.\n");
    ask_new_password(sh, NULL);
}

// Not printed on the console while it's locked (the log is held); `dmesg` in an SSH session and
// the console after the next login show it.
static void console_failed(unsigned failures) {
    log_msg(SOS_LOG_AUTH | SOS_LOG_WARNING,
            "console: wrong password (%u since the last right one)", failures);
}

static void console_auto_locked(const char *why) {
    log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "console: locked (%s)", why);
}

static const login_hooks_t console_login = {
    console_now_ms, settings_console_has_password, settings_console_check_begin,
    console_check_step, settings_console_check_abort, console_no_keys, console_rescue_allowed,
    console_opened, console_first_use, console_failed, console_auto_locked,
    failflash_count, failflash_attempt, failflash_clear,    // the count is kept across reboots
};

static void check_tables(void);

void shell_console_start(shell_t *console) {
    check_tables();
    if (!console_no_keys() && !settings_console_has_password())
        log_msg(SOS_LOG_AUTH | SOS_LOG_WARNING,
                "console: no console password is set: the console is open to "
                "anyone at the port (`passwd` sets one)");
    // Wrong answers from before this boot still count: say so, since the first check will wait.
    unsigned kept = failflash_count();
    if (kept && settings_console_has_password())
        log_msg(SOS_LOG_AUTH | SOS_LOG_WARNING,
                "console: %u wrong password%s since the last right one, "
                "counted across reboots; the next check waits for it",
                kept, kept == 1 ? "" : "s");
    login_start(console, &console_login);
}

// --- setup: guided configuration (console only) --------------------------------------------------

static struct {
    char ssid[CONFIG_SSID_MAX + 1];
    char password[CONFIG_PASSWORD_MAX + 1];
    bool wifi_changed;
} setup_state;

static void setup_finish(shell_t *sh) {
    sos_config_t *c = settings();
    bool wifi_changed = setup_state.wifi_changed;
    if (wifi_changed)       // checked as it was typed
        config_wifi_set_first(c, setup_state.ssid, setup_state.password);
    memset(&setup_state, 0, sizeof(setup_state));
    save(sh);
    log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: setup completed");
    shell_printf(sh, "\nsaved:\n");
    cmd_config(sh, NULL);
    if (!c->key_count)
        shell_printf(sh, "\nno trusted keys yet: add one with 'key add' to allow SSH logins\n");
    if (wifi_changed)
        wifi_reconnect();           // a new network: don't drop a working link otherwise
}

static void on_setup_key(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (!answer) {
        shell_printf(sh, "setup cancelled; nothing changed\n");
        return;
    }
    if (*answer && !key_add_line(sh, answer)) {
        shell_ask(sh, "Public key to trust (empty to skip): ", false, on_setup_key, NULL);
        return;                                 // try again
    }
    setup_finish(sh);
}

static void ask_setup_key(shell_t *sh) {
    shell_printf(sh, "\nTrusted keys: %u. Paste a public key to trust (one line from\n"
                     "~/.ssh/id_ed25519.pub on your computer), or press Enter to skip.\n",
                 settings()->key_count);
    shell_ask(sh, "Public key: ", false, on_setup_key, NULL);
}

static void on_setup_password(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (!answer) {
        shell_printf(sh, "setup cancelled; nothing changed\n");
        return;
    }
    const char *err = config_check_password(answer);
    if (err) {
        shell_printf(sh, "%s\n", err);
        shell_ask(sh, "Wi-Fi password (empty for an open network): ", true, on_setup_password, NULL);
        return;
    }
    strcpy(setup_state.password, answer);
    memset(answer, 0, strlen(answer));
    setup_state.wifi_changed = true;
    ask_setup_key(sh);
}

static void on_setup_ssid(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    wifi_names_stop();
    if (!answer) {
        shell_printf(sh, "setup cancelled; nothing changed\n");
        return;
    }
    if (!*answer && settings()->wifi_ssid[0]) {      // keep the current network
        ask_setup_key(sh);
        return;
    }
    const char *err = config_check_ssid(answer);
    if (err) {
        shell_printf(sh, "%s\n", err);
        ask_ssid(sh, "Wi-Fi network (SSID): ", on_setup_ssid, NULL);
        return;
    }
    strcpy(setup_state.ssid, answer);
    shell_ask(sh, "Wi-Fi password (empty for an open network): ", true, on_setup_password, NULL);
}

// --- the console password ------------------------------------------------------------------------

// Asking for a new password, twice, then what follows (NULL: nothing).
static char new_password[CONFIG_CONSOLE_PW_MAX + 1];
static void (*after_password)(shell_t *sh);

static void wipe_text(char *s, size_t size) {
    volatile char *v = s;
    while (size--)
        *v++ = 0;
}

static void on_new_password(shell_t *sh, char *answer, void *arg);

static void ask_new_password(shell_t *sh, void (*then)(shell_t *sh)) {
    after_password = then;
    shell_ask(sh, "New console password (8 to 64 characters): ", true, on_new_password, NULL);
}

static void on_new_password_again(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    void (*then)(shell_t *sh) = after_password;
    if (!answer) {
        wipe_text(new_password, sizeof(new_password));
        shell_printf(sh, "cancelled; the console password is unchanged\n");
        return;
    }
    bool same = !strcmp(answer, new_password);
    wipe_text(answer, strlen(answer));
    if (!same) {
        wipe_text(new_password, sizeof(new_password));
        shell_printf(sh, "they differ; try again\n");
        ask_new_password(sh, then);
        return;
    }
    const char *err = settings_console_set_password(new_password);
    wipe_text(new_password, sizeof(new_password));
    if (err) {
        shell_printf(sh, "not set: %s\n", err);
        return;
    }
    save(sh);
    log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: console password set");
    // Whoever set it had the console open. Wrong answers counted against an earlier password
    // (or with none set) must not delay the first login with this one: the count kept in flash
    // and the one login.c took from it at boot both start again.
    login_password_set();
    shell_printf(sh, "console password set; the console asks for it from now on (`logout` locks "
                     "it)\n");
    if (then)
        then(sh);
}

static void on_new_password(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (!answer) {
        shell_printf(sh, "cancelled; the console password is unchanged\n");
        return;
    }
    const char *err = config_check_console_password(answer);
    if (err) {
        wipe_text(answer, strlen(answer));
        shell_printf(sh, "%s\n", err);
        ask_new_password(sh, after_password);
        return;
    }
    strcpy(new_password, answer);
    wipe_text(answer, strlen(answer));
    shell_ask(sh, "Again: ", true, on_new_password_again, NULL);
}

// The current password, checked in one go (a few hundred milliseconds), before changing it: an
// unlocked console left alone shouldn't be enough to change the password.
static void on_current_password(shell_t *sh, char *answer, void *arg) {
    (void)arg;
    if (!answer)
        return;
    int r = settings_console_check_begin(answer) ? 0 : -1;
    wipe_text(answer, strlen(answer));
    while (r == 0)
        r = settings_console_check_step();
    if (r != 1) {
        shell_printf(sh, "wrong password\n");
        log_msg(SOS_LOG_AUTH | SOS_LOG_WARNING,
                "console: wrong current password given to `passwd`");
        return;
    }
    ask_new_password(sh, NULL);
}

static void cmd_passwd(shell_t *sh) {
    if (!sh->io->admin) {
        shell_printf(sh, "the console password can only be changed on the serial console\n");
        return;
    }
    if (settings_console_has_password())
        shell_ask(sh, "Current console password: ", true, on_current_password, NULL);
    else
        ask_new_password(sh, NULL);
}

static void setup_network(shell_t *sh);

static void cmd_setup(shell_t *sh) {
    if (!require_admin(sh))
        return;
    shell_printf(sh, "SOS setup. Press Enter to keep the value in brackets, Ctrl-C to cancel.\n");
    // The console password comes first: a board isn't left set up with an open console.
    if (!settings_console_has_password()) {
        shell_printf(sh, "This board has no console password yet. It's asked for before the "
                         "console takes any\ncommand. If it's forgotten, the only way back in is "
                         "to erase the configuration over USB.\n");
        ask_new_password(sh, setup_network);
        return;
    }
    setup_network(sh);
}

static void setup_network(shell_t *sh) {
    static char prompt[CONFIG_SSID_MAX + 40];
    memset(&setup_state, 0, sizeof(setup_state));
    if (settings()->wifi_ssid[0])
        snprintf(prompt, sizeof(prompt), "Wi-Fi network (SSID) [%s]: ", settings()->wifi_ssid);
    else
        snprintf(prompt, sizeof(prompt), "Wi-Fi network (SSID): ");
    ask_ssid(sh, prompt, on_setup_ssid, NULL);
}

static void show_ntp_sync_hours(shell_t *sh) {
    unsigned h = settings()->ntp_sync_hours;
    if (h)
        shell_printf(sh, "ntp sync: every %u h\n", h);
    else
        shell_printf(sh, "ntp sync: off (RTC only; 'time sync' syncs once)\n");
}

// `time auto [off | <hours>]`: periodic NTS sync (a setting, so console only to change).
static void cmd_time_auto(shell_t *sh, const char *value) {
    while (*value == ' ')
        value++;
    if (!*value) {
        show_ntp_sync_hours(sh);
        return;
    }
    if (!require_admin(sh))
        return;
    unsigned long hours = 0;
    if (strcmp(value, "off")) {
        char *end;
        hours = strtoul(value, &end, 10);
        if (*end || end == value || hours < 1 || hours > CONFIG_NTP_SYNC_HOURS_MAX) {
            shell_printf(sh, "not changed: give 'off' or hours from 1 to %u\n",
                         CONFIG_NTP_SYNC_HOURS_MAX);
            return;
        }
    }
    settings()->ntp_sync_hours = (uint16_t)hours;
    save(sh);
    if (hours)
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: ntp sync every %lu h", hours);
    else
        log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: ntp sync off");
    show_ntp_sync_hours(sh);
}

// `time cal [off | <ppm>]`: RTC calibration (a setting, so console only to change). The value is
// how fast the RTC runs, in ppm (negative: slow); the RTC is then slowed, or sped up, by the
// nearest amount its offset register can do.
static void cmd_time_cal(shell_t *sh, const char *value) {
    char cal[40];
    while (*value == ' ')
        value++;
    if (!*value) {
        timesync_cal_str(cal, sizeof(cal));
        shell_printf(sh, "rtc calibration: %s\n", cal);
        return;
    }
    if (!require_admin(sh))
        return;
    int32_t mppm = 0;
    int steps = 0;
    if (strcmp(value, "off") &&
        (!timeconv_parse_ppm(value, &mppm) || !timeconv_cal_steps(mppm, &steps))) {
        shell_printf(sh, "not changed: give 'off', or how fast the RTC runs in ppm, from %d to %d "
                         "(negative: slow)\n",
                     TIMECONV_CAL_STEPS_MIN * TIMECONV_CAL_STEP_MPPM / 1000,
                     TIMECONV_CAL_STEPS_MAX * TIMECONV_CAL_STEP_MPPM / 1000);
        return;
    }
    settings()->rtc_offset = (int8_t)steps;
    save(sh);
    bool applied = timesync_apply_cal();
    timesync_cal_str(cal, sizeof(cal));
    log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: rtc calibration %s", cal);
    shell_printf(sh, "rtc calibration: %s%s\n", cal,
                 applied ? "" : " (saved, but the RTC didn't take it)");
}

static void show_ntp_server(shell_t *sh) {
    const char *s = settings()->ntp_server;
    if (*s)
        shell_printf(sh, "nts server: %s\n", s);
    else
        shell_printf(sh, "nts server: %s (build default)\n", SOS_NTS_SERVER);
}

// `time server [<name> | default]`: NTS server (a setting, so console only to change). Its
// certificate chain must end at a built-in root (src/tls/tls_roots.c).
static void cmd_time_server(shell_t *sh, const char *value) {
    while (*value == ' ')
        value++;
    if (!*value) {
        show_ntp_server(sh);
        return;
    }
    if (!require_admin(sh))
        return;
    sos_config_t *c = settings();
    if (!strcmp(value, "default")) {
        c->ntp_server[0] = '\0';
    } else {
        const char *err = config_check_ntp_server(value);
        if (err) {
            shell_printf(sh, "not changed: %s\n", err);
            return;
        }
        snprintf(c->ntp_server, sizeof(c->ntp_server), "%s", value);
    }
    save(sh);
    log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, "config: nts server set to '%s'",
            c->ntp_server[0] ? c->ntp_server : SOS_NTS_SERVER);
    show_ntp_server(sh);
    shell_printf(sh, "used from the next sync ('time sync' to sync now)\n");
}

// Clock sync is allowed from SSH too: it can't lock anyone out, and the RTC drifts.
static void cmd_time(shell_t *sh, char *args) {
    if (!args)
        timesync_status(sh);
    else if (!strcmp(args, "sync"))
        shell_printf(sh, timesync_request_nts()
                     ? "nts: sync started; 'time' shows the result\n"
                     : "nts: a sync is already running\n");
    else if (!strcmp(args, "sync sntp")) {
        // Unauthenticated: whoever answers sets the clock, which TLS checks certificate dates
        // against. So only on the console, like the configuration.
        if (!require_admin(sh))
            return;
        shell_printf(sh, timesync_request_sntp()
                     ? "sntp: unauthenticated sync from " SOS_NTP_SERVER
                       " started; 'time' shows the result\n"
                     : "a sync is already running\n");
    } else if (!strncmp(args, "auto", 4) && (args[4] == ' ' || !args[4]))
        cmd_time_auto(sh, args + 4);
    else if (!strncmp(args, "server", 6) && (args[6] == ' ' || !args[6]))
        cmd_time_server(sh, args + 6);
    else if (!strncmp(args, "cal", 3) && (args[3] == ' ' || !args[3]))
        cmd_time_cal(sh, args + 3);
    else if (!strncmp(args, "zone", 4) && (args[4] == ' ' || !args[4]))
        tzzone_command(sh, args + 4);
    else
        shell_printf(sh, "usage: time [sync [sntp] | auto [off | <hours>] | server [<name> | default] | "
                         "cal [off | <ppm>] | zone [<name> | utc | list [<prefix>] | install]]\n");
}

// `nts` / `nts ke [host]` / `nts query`: NTS key exchange and authenticated time queries
// (development: queries report the offset but don't set the clock yet).
// `tls roots [approve | forget]`: extra TLS roots from the SD card (src/tls/certstore.c).
static void cmd_tls(shell_t *sh, char *args) {
    if (args && !strncmp(args, "roots", 5) && (args[5] == ' ' || !args[5])) {
        char *rest = args + 5;
        while (*rest == ' ')
            rest++;
        certstore_command(sh, *rest ? rest : NULL);
    } else {
        shell_printf(sh, "usage: tls roots [approve | forget]\n");
    }
}

static void cmd_nts(shell_t *sh, char *args) {
    if (!args) {
        nts_status(sh);
        return;
    }
    if (!strcmp(args, "query")) {
        const char *err = nts_query_start();
        if (err)
            shell_printf(sh, "nts: %s\n", err);
        else
            shell_printf(sh, "nts: query started; 'nts' shows the result\n");
        return;
    }
    if (strncmp(args, "ke", 2) || (args[2] != ' ' && args[2])) {
        shell_printf(sh, "usage: nts [ke [<host>] | query]\n");
        return;
    }
    const char *host = args + 2;
    while (*host == ' ')
        host++;
    // Another host makes the device connect out to it: console only. A session may run the
    // exchange with the build-time default server (SOS_NTS_SERVER).
    if (*host && !require_admin(sh))
        return;
    if (*host && config_check_ntp_server(host)) {
        shell_printf(sh, "bad host name: %s\n", config_check_ntp_server(host));
        return;
    }
    shell_printf(sh, nts_ke_start(*host ? host : NULL)
                 ? "nts: key exchange started; 'nts' shows the result\n"
                 : "nts: a key exchange or query is already running\n");
}

// --- dispatch ------------------------------------------------------------------------------------

// The pipeline stages (shell/pipe.h) aren't commands: typed first on a line, they say so.
static void run_stage(shell_t *sh, const char *name, const char *form) {
    shell_printf(sh, "%s: only after a pipe (<command> | %s)\n", name, form);
}
static void run_grep(shell_t *sh, char *args) { (void)args; run_stage(sh, "grep", "grep <text>"); }
static void run_head(shell_t *sh, char *args) { (void)args; run_stage(sh, "head", "head [<n>]"); }
static void run_tail(shell_t *sh, char *args) { (void)args; run_stage(sh, "tail", "tail [<n>]"); }
static void run_tee(shell_t *sh, char *args) { (void)args; run_stage(sh, "tee", "tee [-a] <path>"); }
static void run_wc(shell_t *sh, char *args) { (void)args; run_stage(sh, "wc", "wc"); }

// Commands that take no arguments, and others whose handler has another signature.
static void run_help(shell_t *sh, char *args) { (void)args; cmd_help(sh); }
// "12 s", "14 min", "3 h", or "?" for unknown.
static void format_up(char *buf, size_t len, uint32_t s) {
    if (s == HISTORY_NONE)
        snprintf(buf, len, "?");
    else if (s < 120)
        snprintf(buf, len, "%lu s", (unsigned long)s);
    else if (s < 7200)
        snprintf(buf, len, "%lu min", (unsigned long)(s / 60));
    else
        snprintf(buf, len, "%lu h", (unsigned long)(s / 3600));
}

// `uptime history`: the history sector (core/history.h), newest first: each boot with its reason
// and how long the board had been up before it, and each time sync that set the RTC.
static void cmd_uptime_history(shell_t *sh) {
    const uint8_t *sector = history_sector();
    history_scan_t s;
    history_scan(sector, &s);
    if (s.state != HISTORY_OURS) {
        shell_printf(sh, "%s\n", s.state == HISTORY_ABSENT ? "no history: the image overlaps its sector"
                                 : s.state == HISTORY_NEWER ? "the history sector has a newer format"
                                                            : "nothing recorded yet");
        return;
    }
    shell_printf(sh, " boot  when (UTC)           reason            up before\n");
    for (unsigned i = s.next; i-- > 0;) {
        history_rec_t r;
        if (!history_slot(sector, i, &r)) {
            shell_printf(sh, "    -  (damaged record)\n");
            continue;
        }
        char when[24] = "(clock not set)", up[16], cal[16];
        if (r.wall != HISTORY_NONE)
            timeconv_format(r.wall, when, sizeof(when));
        if (r.kind == HISTORY_KIND_RTC) {
            timeconv_format_cal(cal, sizeof(cal), r.cal_steps);
            shell_printf(sh, "    -  %-19s  rtc set (%s)%s\n", when, cal,
                         r.confirmed ? "" : ", not taken");
        } else if (r.kind == HISTORY_KIND_BOOT) {
            format_up(up, sizeof(up), r.uptime_s);
            shell_printf(sh, "%5u  %-19s  %-16s  %s\n", r.boot_no, when,
                         resetwhy_name((resetwhy_t)r.reason), up);
        } else {
            shell_printf(sh, "    -  (test record)\n");
        }
    }
    shell_printf(sh, "%u of %u records used%s\n", s.next, (unsigned)HISTORY_SLOTS,
                 s.damaged || s.stray ? "; some damaged (see health)" : "");
}

static void run_uptime(shell_t *sh, char *args) {
    if (args && !strcmp(args, "history"))
        cmd_uptime_history(sh);
    else if (args)
        shell_printf(sh, "usage: uptime [history]\n");
    else
        cmd_uptime(sh);
}
static void run_power(shell_t *sh, char *args) { (void)args; cmd_power(sh); }
static void run_version(shell_t *sh, char *args) { (void)args; cmd_version(sh); }
static void run_mem(shell_t *sh, char *args) {
    if (!args) {
        cmd_mem(sh);
        return;
    }
#ifdef SOS_TEST_COMMANDS
    cmd_mem_test(sh, args);
#else
    shell_printf(sh, "usage: mem\n");
#endif
}
static void run_net(shell_t *sh, char *args) { (void)args; cmd_net(sh); }
static void run_ssh(shell_t *sh, char *args) { (void)args; sshd_status(sh); }
static void run_bootkey(shell_t *sh, char *args) { (void)args; cmd_bootkey(sh); }
static void run_rootsecret(shell_t *sh, char *args) { cmd_rootsecret(sh, args); }
static void run_passwd(shell_t *sh, char *args) { (void)args; cmd_passwd(sh); }
static void run_setup(shell_t *sh, char *args) { (void)args; cmd_setup(sh); }
static void run_logout(shell_t *sh, char *args) { (void)args; shell_logout(sh); }

static void run_hostname(shell_t *sh, char *args) {
    set_field(sh, "hostname", args, settings()->hostname, sizeof(settings()->hostname),
              config_check_hostname);
}

#ifdef SOS_TEST_COMMANDS
// `crash brownout` (test builds only, docs/design/health.md §9.5): a real brownout reset, by
// raising the brownout detector's threshold to 1.204 V, above the core supply's 1.1 V. The reset
// puts the register back to its default (0.946 V), so the next boot is ordinary, and the boot
// says `last reset: brownout`. Like a power dip it clears the POWMAN and watchdog scratch
// registers. Refused while anything writes the card or flash: open files, sd check or a format, a
// Lua script (it writes through core 0). Flash writes are synchronous on core 0 (flashop.h), so
// none is in progress while a command runs; the log files are flushed first, as `reboot` does.
static void crash_brownout(shell_t *sh) {
    sdcard_health_t sd;
    sdcard_health_state(&sd);
    const char *busy = files_busy() ? "files are open (SFTP, cp, tee or a > redirection)"
                     : sd.busy ? sd.busy
                     : lua_host_running() ? "a Lua script is running"
                     : NULL;
    if (busy) {
        shell_printf(sh, "crash brownout: refused: %s\n", busy);
        return;
    }
    log_msg(SOS_LOG_KERN | SOS_LOG_NOTICE, "crash brownout (test): the detector's threshold raised "
            "above the core supply");
    logfile_flush();
    // Holds the main loop on purpose, as wdog_reboot() does: the flush above hands the log lines
    // to the card, and this gives them time to reach it before the reset loses what's in RAM.
    sleep_ms(100);
    uint32_t was = powman_hw->bod;
    powman_hw->bod = POWMAN_PASSWORD_BITS | (0x11u << POWMAN_BOD_VSEL_LSB) | POWMAN_BOD_EN_BITS;
    busy_wait_ms(100);                  // the detector acts within about 40 us
    powman_hw->bod = POWMAN_PASSWORD_BITS | (was & 0xffffu);
    shell_printf(sh, "crash brownout: the detector didn't reset the chip (BOD 0x%08lx put back)\n",
                 (unsigned long)was);
}
#endif

static void run_crash(shell_t *sh, char *args) {
    // Testing the fault record (core/fault.h): stops the firmware on purpose.
    if (!require_admin(sh))
        return;
#ifdef SOS_TEST_COMMANDS
    if (args && !strcmp(args, "brownout")) {
        crash_brownout(sh);
        return;
    }
#endif
    if (args)
        fault_test(args);
    shell_printf(sh, "usage: crash fault|stack|panic|hang  (testing: stops the firmware)\n");
}

static void run_reboot(shell_t *sh, char *args) {
    (void)args;
    shell_printf(sh, "rebooting...\n");
    const char *who = chat_name(sh);
    log_msg(SOS_LOG_KERN | SOS_LOG_NOTICE, "reboot (from %s%s%s)", sh->io->name,
            who && strcmp(who, sh->io->name) ? " " : "",
            who && strcmp(who, sh->io->name) ? who : "");
    wdog_reboot();
}

// --- health: self-checks (core/health.h) ---

#define HEALTH_JOB_STEP_US 20000        // work per job step, as sd check's CHECK_STEP_US
#define HEALTH_JOB_ROOM    256          // output room wanted before a step (a result line)

static uint64_t health_job_t0;

static void health_print_row(shell_t *sh, unsigned index) {
    char row[HEALTH_ROW_MAX];
    health_format_row(row, sizeof(row), index);
    shell_printf(sh, "%s\n", row);
}

static void health_job_done(unsigned index, void *arg) {
    health_print_row(arg, index);
}

static bool health_job(shell_t *sh, void *arg, bool cancel) {
    (void)arg;
    if (cancel) {
        health_job_step(0, true);
        shell_printf(sh, "stopped\n");
        return false;
    }
    if (shell_write_space(sh) < HEALTH_JOB_ROOM)
        return true;                    // wait for the terminal to take the lines so far
    if (health_job_step(HEALTH_JOB_STEP_US, false))
        return true;
    shell_printf(sh, "done in %u ms\n", (unsigned)((time_us_64() - health_job_t0) / 1000));
    return false;
}

#ifdef SOS_TEST_COMMANDS
// `health inject <name> fail|clear` and `health inject gate keys|crypto`, test builds only: set
// a result by hand, to see the refusals of docs/design/health.md §6 on the board.
static void health_inject_cmd(shell_t *sh, const char *what, const char *how) {
    bool ok;
    if (what && how && !strcmp(what, "gate"))
        ok = health_board_inject_gate(how);
    else if (what && how && !strcmp(how, "fail"))
        ok = health_inject(what, HEALTH_FAIL, "set by hand (health inject)");
    else if (what && how && !strcmp(how, "clear"))
        ok = health_inject(what, HEALTH_PASS, "cleared by hand (health inject)");
    else
        ok = false;
    if (!ok) {
        shell_printf(sh, "usage: health inject <name> fail|clear | health inject gate keys|crypto\n");
        return;
    }
    health_print_row(sh, (unsigned)health_find(!strcmp(what, "gate")
                                               ? (!strcmp(how, "keys") ? "inj-keys"
                                                                       : "inj-crypto")
                                               : what));
}

static void health_print_line(void *arg, const char *line) {
    shell_printf(arg, "%s\n", line);
}

// state-dump's lines are long (up to 256 bytes): wrapped at the terminal's usual 80 columns.
static void health_print_wrapped(void *arg, const char *line) {
    statecheck_wrap(line, STATECHECK_WRAP_COLS, health_print_line, arg);
}
#endif

static void cmd_health(shell_t *sh, char *args) {
    char row[HEALTH_ROW_MAX];
    if (!args) {
        health_format_header(row, sizeof(row));
        shell_printf(sh, "%s\n", row);
        for (unsigned i = 0; i < health_count(); i++)
            health_print_row(sh, i);
        shell_printf(sh, "boot checks: %u ms\n", (unsigned)health_boot_ms());
        return;
    }
    char *w1 = strtok(args, " "), *w2 = strtok(NULL, " "), *w3 = strtok(NULL, " ");
    char *w4 = strtok(NULL, " ");
    bool run = w1 && !strcmp(w1, "run") && !w3, full = w1 && !strcmp(w1, "full") && !w2;
#ifdef SOS_TEST_COMMANDS
    bool inject = w1 && !strcmp(w1, "inject") && !w4;
    bool times = w1 && !strcmp(w1, "crypto-times") && !w2;
    bool diag = w1 && !strcmp(w1, "trng-diag") && !w2;
    // What the checks of stored state and of the background tier read (docs/design/health.md
    // §9.3, §9.5). It only reads and prints no secret, so any shell may: the console may be
    // locked at its password prompt.
    if (w1 && !strcmp(w1, "state-dump") && !w2) {
        statecheck_board_dump(health_print_wrapped, sh);
        bgcheck_board_dump(health_print_wrapped, sh);
        fullcheck_board_dump(health_print_wrapped, sh);
        countcheck_board_dump(health_print_wrapped, sh);
        return;
    }
    bool walk = w1 && !strcmp(w1, "heap-walk") && !w2;
    bool fill = w1 && !strcmp(w1, "history-fill") && !w2;
    // `health fault wifi timeout|bus <n>` takes a fourth word; the others at most three.
    bool fault_wifi = w1 && !strcmp(w1, "fault") && w2 && !strcmp(w2, "wifi") &&
                      !strtok(NULL, " ");
    bool fault = w1 && !strcmp(w1, "fault") && w2 && (!w4 || fault_wifi);
#else
    bool inject = false, times = false, diag = false, walk = false, fill = false, fault = false;
    (void)w4;
#endif
    if (!run && !full && !inject && !times && !diag && !walk && !fill && !fault) {
        shell_printf(sh, "usage: health [run [<name>] | full]\n");
        return;
    }
    // They take the CPU for a while (and later phases' full checks, the card): console only.
    if (!sh->io->admin) {
        shell_printf(sh, "checks can only be run on the serial console\n");
        return;
    }
#ifdef SOS_TEST_COMMANDS
    if (inject) {
        health_inject_cmd(sh, w2, w3);
        return;
    }
    // §9.7: a fault for the next run of `ram` or `image` only.
    // §9.9: driver errors counted now, as the Wi-Fi driver's hooks count them.
    if (fault) {
        if (fault_wifi && countcheck_board_wifi_fault(w3, w4))
            shell_printf(sh, "counted as the driver's: health run wifi judges them\n");
        else if (!fault_wifi && !w4 && fullcheck_board_arm_fault(w2, w3))
            shell_printf(sh, "armed for the next run of %s\n", w2);
        else
            shell_printf(sh, "usage: health fault ram once|twice | health fault image | "
                             "health fault wifi timeout|bus <n>\n");
        return;
    }
    // Measurements for a board run (docs/design/health.md §9.2): they hold the loop a moment.
    if (times || diag) {
        if (health_job_running()) {
            shell_printf(sh, "checks are already running\n");
            return;
        }
        if (times)
            health_board_crypto_times(health_print_line, sh);
        else
            health_board_trng_diag(health_print_line, sh);
        return;
    }
    // §9.5: newlib's heap walked by hand (holds the allocator's lock up to 8 ms), and test records
    // that fill the history sector so the next boots exercise its erase.
    if (walk) {
        bgcheck_board_heap_walk(health_print_wrapped, sh);
        return;
    }
    if (fill) {
        unsigned n = history_fill();
        shell_printf(sh, "history: %u test record%s written; the next boot's record erases the "
                     "sector and keeps the newest\n", n, n == 1 ? "" : "s");
        return;
    }
#endif
    const char *err = health_job_start(full ? HEALTH_JOB_FULL : HEALTH_JOB_RUN, run ? w2 : NULL,
                                       health_job_done, sh);
    if (err) {
        shell_printf(sh, "%s\n", err);
        return;
    }
    health_job_t0 = time_us_64();
    health_format_header(row, sizeof(row));
    shell_printf(sh, "%s\n", row);
    shell_run_job(sh, health_job, NULL);
}

// --- values Tab asks for (shell.h: SHELL_VALUES_OF) ---

// `key del <n>`: the trusted keys' numbers, as `key list` shows them, with their comments.
static void tab_key_numbers(shell_t *sh, shell_value_fn fn, void *arg) {
    (void)sh;
    const sos_config_t *c = settings();
    char n[4];
    for (unsigned i = 0; i < c->key_count; i++) {
        snprintf(n, sizeof(n), "%u", i + 1);
        fn(n, c->keys[i].comment, arg);
    }
}

// `wifi remove|prefer <n>`: the saved networks' numbers, as `wifi` shows them, with their SSIDs.
static void tab_wifi_numbers(shell_t *sh, shell_value_fn fn, void *arg) {
    (void)sh;
    char n[4];
    config_wifi_net_t net;
    for (unsigned i = 0; config_wifi_get(settings(), i, &net); i++) {
        snprintf(n, sizeof(n), "%u", i + 1);
        fn(n, net.ssid, arg);
    }
}

// NTS servers the board knows: the build's default and the one set with `time server`.
static void nts_servers(shell_value_fn fn, void *arg) {
    const char *set = settings()->ntp_server;
    fn(NTS_DEFAULT_SERVER, "default", arg);
    if (set[0] && strcmp(set, NTS_DEFAULT_SERVER))
        fn(set, "set with 'time server'", arg);
}

// `nts ke <host>` and `time server <name>`: those servers; console only, as another host is.
static void tab_nts_hosts(shell_t *sh, shell_value_fn fn, void *arg) {
    if (sh->io->admin)
        nts_servers(fn, arg);
}

static void tab_time_server(shell_t *sh, shell_value_fn fn, void *arg) {
    if (!sh->io->admin)
        return;
    fn("default", NULL, arg);
    nts_servers(fn, arg);
}

// `ping <host>`: the NTS servers and the gateway, while Wi-Fi is up.
static void tab_ping_hosts(shell_t *sh, shell_value_fn fn, void *arg) {
    (void)sh;
    nts_servers(fn, arg);
    if (!wifi_up())
        return;
    char gw[IP4ADDR_STRLEN_MAX] = "";
    cyw43_arch_lwip_begin();
    if (netif_default && !ip4_addr_isany_val(*netif_ip4_gw(netif_default)))
        ip4addr_ntoa_r(netif_ip4_gw(netif_default), gw, sizeof(gw));
    cyw43_arch_lwip_end();
    if (gw[0])
        fn(gw, "gateway", arg);
}

static const shell_values_t key_numbers = SHELL_VALUES_INIT(tab_key_numbers);
static const shell_values_t wifi_numbers = SHELL_VALUES_INIT(tab_wifi_numbers);
static const shell_values_t nts_hosts = SHELL_VALUES_INIT(tab_nts_hosts);
static const shell_values_t time_servers = SHELL_VALUES_INIT(tab_time_server);
static const shell_values_t ping_hosts = SHELL_VALUES_INIT(tab_ping_hosts);
static const shell_values_t man_topics = SHELL_VALUES_INIT(man_tab_topics);
static const shell_values_t time_zones = SHELL_VALUES_INIT(tzzone_tab);

// `health run <name>`: the checks' names, with their latest status.
static void tab_health_names(shell_t *sh, shell_value_fn fn, void *arg) {
    if (!sh->io->admin)
        return;
    for (unsigned i = 0; i < health_count(); i++) {
        const health_entry_t *e = health_entry(i);
        fn(e->check->name, health_status_name(e->result.status), arg);
    }
}
static const shell_values_t health_names = SHELL_VALUES_INIT(tab_health_names);

// The words Tab offers after each command (shell.h): the subcommands its parser takes.
#define END {0}
#define W(name) {name, NULL, 0}
static const shell_word_t on_off[] = {W("on"), W("off"), END};
static const shell_word_t on_off_admin[] = {
    {"on", NULL, SHELL_ADMIN}, {"off", NULL, SHELL_ADMIN}, END};
static const shell_word_t levels[] = {
    W("err"), W("warning"), W("notice"), W("info"), W("debug"), END};
static const shell_word_t facilities[] = {
    W("none"), W("kern"), W("auth"), W("daemon"), W("user"), END};

static const shell_word_t temp_words[] = {
    {"cal", (const shell_word_t[]){{"off", NULL, SHELL_ADMIN}, END}, 0},
    END};
static const shell_word_t time_words[] = {
    {"sync", (const shell_word_t[]){{"sntp", NULL, SHELL_ADMIN}, END}, 0},
    {"auto", (const shell_word_t[]){{"off", NULL, SHELL_ADMIN}, END}, 0},
    {"server", SHELL_VALUES_OF(time_servers), 0},
    {"cal", (const shell_word_t[]){{"off", NULL, SHELL_ADMIN}, END}, 0},
    {"zone", SHELL_VALUES_OF(time_zones), 0},
    END};
static const shell_word_t tls_words[] = {
    {"roots", (const shell_word_t[]){{"approve", NULL, SHELL_ADMIN}, {"forget", NULL, SHELL_ADMIN},
                                     END}, 0},
    END};
static const shell_word_t nts_words[] = {{"ke", SHELL_VALUES_OF(nts_hosts), 0}, W("query"), END};
static const shell_word_t dmesg_words[] = {{"-f", facilities, 0}, END};
static const shell_word_t logger_words[] = {{"-p", levels, 0}, END};
static const shell_word_t rotate_words[] = {W("every"), W("at"), END};
static const shell_word_t log_words[] = {
    {"console", levels, SHELL_ADMIN}, {"history", levels, SHELL_ADMIN},
    {"file", levels, SHELL_ADMIN}, {"rotate", rotate_words, SHELL_ADMIN}, END};
static const shell_word_t config_words[] = {{"reset", NULL, SHELL_ADMIN}, END};
static const shell_word_t sensitivities[] = {W("0"), W("1"), W("2"), W("3"), END};
static const shell_word_t glitch_words[] = {
    {"watch", sensitivities, SHELL_ADMIN}, {"on", sensitivities, SHELL_ADMIN},
    {"off", NULL, SHELL_ADMIN},
    {"default", NULL, SHELL_ADMIN}, {"trig", NULL, SHELL_ADMIN}, END};
static const shell_word_t rootsecret_words[] = {
    {"forget", NULL, SHELL_ADMIN},
    {"burn", (const shell_word_t[]){W("plan"), W("rehearse"), END}, SHELL_ADMIN},
    END};
static const shell_word_t key_words[] = {
    W("list"), {"add", NULL, SHELL_ADMIN}, {"del", SHELL_VALUES_OF(key_numbers), SHELL_ADMIN},
    END};
static const shell_word_t wifi_words[] = {
    {"stats", (const shell_word_t[]){W("chip"), END}, 0},
    {"set", NULL, SHELL_ADMIN},
    {"add", NULL, SHELL_ADMIN},
    {"remove", SHELL_VALUES_OF(wifi_numbers), SHELL_ADMIN},
    {"prefer", SHELL_VALUES_OF(wifi_numbers), SHELL_ADMIN},
    {"connect", NULL, SHELL_ADMIN},
    {"power", on_off_admin, 0},             // shows the setting anywhere; on|off: console
    {"country", (const shell_word_t[]){W("worldwide"), END}, SHELL_ADMIN},
    {"keepalive", on_off, SHELL_ADMIN},
    {"iovar", NULL, SHELL_ADMIN},
#ifdef SOS_TEST_COMMANDS
    {"fault", (const shell_word_t[]){W("drop"), END}, SHELL_ADMIN | SHELL_HIDDEN},
#endif
    END};
static const shell_word_t sd_words[] = {
    W("info"),
    W("speed"),
    {"test", NULL, SHELL_ADMIN},
    {"format", (const shell_word_t[]){W("exfat"), W("fat32"), W("plain"), END}, SHELL_ADMIN},
    {"unmount", NULL, SHELL_ADMIN},
    {"check", (const shell_word_t[]){W("fix"), END}, SHELL_ADMIN},
#ifdef SOS_TEST_COMMANDS
    {"fault", (const shell_word_t[]){W("fail"), W("reboot"), W("read"), W("off"), END},
     SHELL_ADMIN},
#endif
    END};
static const shell_word_t lua_words[] = {
    W("-e"), {"-b", NULL, SHELL_PATH}, W("ps"), W("stop"), END};
static const shell_word_t url_start[] = {{"https://", NULL, SHELL_NOSPACE}, END};
static const shell_word_t http_words[] = {
    {"get", url_start, 0}, {"head", url_start, 0}, {"post", url_start, 0}, END};
#ifdef SOS_TEST_COMMANDS
static const shell_word_t mem_hold_words[] = {W("leave"), W("holes"), END};
static const shell_word_t mem_release_words[] = {W("first"), END};
static const shell_word_t mem_words[] = {
    {"hold", mem_hold_words, SHELL_ADMIN}, {"release", mem_release_words, SHELL_ADMIN}, END};
#endif
static const shell_word_t health_words[] = {
    {"run", SHELL_VALUES_OF(health_names), SHELL_ADMIN},
    {"full", NULL, SHELL_ADMIN},
#ifdef SOS_TEST_COMMANDS
    {"inject", NULL, SHELL_ADMIN | SHELL_HIDDEN},
    {"state-dump", NULL, SHELL_HIDDEN},
    {"heap-walk", NULL, SHELL_ADMIN | SHELL_HIDDEN},
    {"history-fill", NULL, SHELL_ADMIN | SHELL_HIDDEN},
#endif
    END};
static const shell_word_t crash_words[] = {W("fault"), W("stack"), W("panic"), W("hang"),
#ifdef SOS_TEST_COMMANDS
                                           W("brownout"),
#endif
                                           END};
static const shell_word_t uptime_words[] = {W("history"), END};

static const shell_cmd_t commands[] = {
    {"help", run_help, NULL, NULL, 0},
    {"?", run_help, NULL, NULL, SHELL_HIDDEN},
    {"uptime", run_uptime, NULL, uptime_words, 0},
    {"temp", cmd_temp, NULL, temp_words, 0},
    {"power", run_power, NULL, NULL, 0},
    {"version", run_version, NULL, NULL, 0},
    {"time", cmd_time, NULL, time_words, 0},
    {"tls", cmd_tls, NULL, tls_words, 0},
    {"nts", cmd_nts, NULL, nts_words, 0},
#ifdef SOS_TEST_COMMANDS
    {"mem", run_mem, NULL, mem_words, 0},
#else
    {"mem", run_mem, NULL, NULL, 0},
#endif
    {"net", run_net, NULL, NULL, 0},
    {"ping", ping_command, NULL, SHELL_VALUES_OF(ping_hosts), 0},
    {"ssh", run_ssh, NULL, NULL, 0},
    {"who", chat_cmd_who, NULL, NULL, 0},
    {"wall", chat_cmd_wall, NULL, NULL, 0},
    {"chat", chat_cmd_chat, NULL, NULL, 0},
    {"dmesg", cmd_dmesg, NULL, dmesg_words, 0},
    {"logger", cmd_logger, NULL, logger_words, 0},
    {"log", cmd_log, NULL, log_words, 0},
    {"config", cmd_config, NULL, config_words, 0},
    {"glitch", cmd_glitch, NULL, glitch_words, 0},
    {"health", cmd_health, NULL, health_words, 0},
    {"bootkey", run_bootkey, NULL, NULL, 0},
    {"rootsecret", run_rootsecret, NULL, rootsecret_words, 0},
    {"key", cmd_key, NULL, key_words, 0},
    {"wifi", cmd_wifi, NULL, wifi_words, 0},
    {"hostname", run_hostname, NULL, NULL, 0},
    {"sd", sdcard_command, NULL, sd_words, 0},
    {"lua", NULL, lua_command, lua_words, SHELL_PATH},
    {"edit", NULL, edit_command, NULL, SHELL_PATH},
    {"more", NULL, view_command, NULL, SHELL_PATH},
    {"man", man_command, NULL, SHELL_VALUES_OF(man_topics), 0},
    {"http", http_command, NULL, http_words, 0},
    {"fetch", NULL, fetch_command, NULL, SHELL_PATH},
    {"passwd", run_passwd, NULL, NULL, SHELL_ADMIN},
    {"setup", run_setup, NULL, NULL, SHELL_ADMIN},
    {"logout", run_logout, NULL, NULL, 0},
    {"exit", run_logout, NULL, NULL, 0},
    {"crash", run_crash, NULL, crash_words, SHELL_ADMIN | SHELL_HIDDEN},
    {"grep", run_grep, NULL, NULL, SHELL_HIDDEN},     // pipeline stages: Tab offers them after "| "
    {"head", run_head, NULL, NULL, SHELL_HIDDEN},
    {"tail", run_tail, NULL, NULL, SHELL_HIDDEN},
    {"tee", run_tee, NULL, NULL, SHELL_HIDDEN},
    {"wc", run_wc, NULL, NULL, SHELL_HIDDEN},
    {"reboot", run_reboot, NULL, NULL, 0},
    END};
#undef W
#undef END

static const shell_cmd_t *const command_tables[] = {commands, files_commands, sums_commands,
                                                      gzip_commands, tar_commands, NULL};

// Debug builds, once at startup: every command has one handler, and those that take paths take
// words (shell.h). The host tests check files_commands; this checks commands[] too.
static void check_tables(void) {
#ifndef NDEBUG
    const shell_cmd_t *bad = shell_tables_check(command_tables);
    if (bad)
        log_msg(SOS_LOG_KERN | SOS_LOG_ERR, "shell: command `%s` breaks the table rule (one of "
                "run and runv; runv for SHELL_PATH)", bad->name);
#endif
}

const shell_cmd_t *const *shell_command_tables(const shell_t *sh) {
    if (sh->io->admin && login_locked())        // nothing to offer before the password
        return NULL;
    return command_tables;
}

void shell_page(shell_t *sh, const char *name, char *text, size_t len) {
    edit_view_take(sh, name, text, len);
}

bool shell_page_growing(shell_t *sh, const char *name, char *text, size_t len) {
    return edit_view_growing(sh, name, text, len);
}

bool shell_execute(shell_t *sh, int argc, char **argv) {
    // The console's lock was checked on the raw line (run_line(), login_gate()).
    // A `runv` handler gets the words; a `run` handler the rest joined, single-spaced (NULL
    // without any).
    const char *cmd = argv[0];
    for (const shell_cmd_t *const *t = command_tables; *t; t++)
        for (const shell_cmd_t *c = *t; c->name; c++)
            if (!strcmp(cmd, c->name)) {
                if (c->runv)
                    c->runv(sh, argc, argv);
                else
                    c->run(sh, words_join(argc, argv));
                return true;
            }
    return false;                           // run_line() says so, on the terminal
}
