// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Julio Martinez

#include <stdio.h>
#include <time.h>

#include "hardware/sync.h"
#include "pico/cyw43_arch.h"
#include "pico/stdio/driver.h"
#include "pico/stdlib.h"

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/wc_port.h>
#include <wolfssh/ssh.h>

#include "chat/chat.h"
#include "core/fault.h"
#include "core/glitchdet.h"
#include "core/keyprov.h"
#include "core/log.h"
#include "core/failflash.h"
#include "core/rootsource.h"
#include "core/consoleonly.h"
#include "core/sensors.h"
#include "core/settings.h"
#include "core/stackmon.h"
#include "core/wdog.h"
#include "files/cardkey.h"
#include "lua/lua_host.h"
#include "net/wifi.h"
#include "shell/login.h"
#include "shell/shell.h"
#include "ssh/hostkey.h"
#include "ssh/sshd.h"
#include "time/nts.h"
#include "time/timesync.h"
#include "tls/tls.h"

// Minutes without a key typed after which an open console locks itself. 0: never. A CMake
// setting (-DSOS_CONSOLE_IDLE_LOCK_MIN=..., or local.cmake).
#ifndef SOS_CONSOLE_IDLE_LOCK_MIN
#define SOS_CONSOLE_IDLE_LOCK_MIN 10
#endif

// --- console -------------------------------------------------------------------------------------

// The USB/UART console shell. stdio already translates "\n" to "\r\n".
// The gate. The console is stdio (USB and UART), and so is every printf in the libraries this
// firmware is built from: the Wi-Fi driver, for one, prints its firmware banner and the MAC
// address at boot. A locked console must show none of that (issue #64), and the log's hold
// only covers what goes through the log. So while the console is locked, stdio as a whole is
// closed: the SDK sends output only to the driver selected with stdio_filter_driver(), and this
// one is never registered, so nothing is sent anywhere and nothing is read. The shell's own
// reads and writes below open the gate for just as long as they take.
static stdio_driver_t console_gate;

static void console_gate_set(bool closed) {
    stdio_filter_driver(closed ? &console_gate : NULL);
}

static int console_read(void *ctx, char *buf, int len) {
    (void)ctx;
    int n = 0;
    int c;
    console_gate_set(false);
    while (n < len && (c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT)
        buf[n++] = (char)c;
    console_gate_set(login_locked());
    if (n)
        login_activity();               // someone is typing: the idle lock starts over
    return n;
}

static void console_write(void *ctx, const char *buf, int len) {
    (void)ctx;
    console_gate_set(false);
    fwrite(buf, 1, (size_t)len, stdout);
    fflush(stdout);
    console_gate_set(login_locked());
}

static void console_logout(void *ctx);

static const shell_io_t console_io = {
    .name = "console",
    .read = console_read,
    .write = console_write,
    .logout = console_logout,   // locks the console again (shell/login.h)
    .crlf = false,
    .admin = true,              // physical access: may change the configuration
};

static shell_t console_shell;   // io is NULL until shell_bind(): the log won't redraw before that

static void console_logout(void *ctx) {
    (void)ctx;
    login_lock(&console_shell);
}

// --- log hooks -----------------------------------------------------------------------------------

static uint64_t log_now(void) { return time_us_64(); }
static bool log_in_irq(void) { return __get_current_exception() != 0; }
static uint32_t log_lock(void) { return save_and_disable_interrupts(); }
static void log_unlock(uint32_t saved) { restore_interrupts(saved); }
static void log_write(const char *s, size_t n) { console_write(NULL, s, (int)n); }
static void log_hide(void) { shell_hide_input(&console_shell); }
static void log_show(void) { shell_show_input(&console_shell); }
// Nothing is printed while a full-screen program (the editor) has the console, nor while the
// console is locked: log lines can name networks, addresses and users. They wait in the history
// and are printed after a login. The console counts as locked from power-on until the boot code
// knows whether a password is set (shell/login.h), so boot messages wait too.
static bool log_hold(void) { return shell_raw_mode(&console_shell) || login_locked(); }

static const log_hooks_t log_hooks = {
    log_now, log_in_irq, log_lock, log_unlock, log_write, log_hide, log_show, log_hold,
};

// --- chat hooks ----------------------------------------------------------------------------------

static uint64_t chat_now(void) { return time_us_64(); }

// Message times: UTC hours and minutes, as `time` shows, or time since boot before the clock
// is set.
static void chat_stamp(char *buf, size_t len) {
    if (timesync_clock_set()) {
        time_t t = time(NULL);
        struct tm tm;
        gmtime_r(&t, &tm);
        snprintf(buf, len, "%02d:%02d", tm.tm_hour, tm.tm_min);
    } else {
        uint64_t m = time_us_64() / 60000000;
        snprintf(buf, len, "+%lluh%02llum", m / 60, m % 60);
    }
}

static const chat_hooks_t chat_hooks = { chat_now, chat_stamp };

// --- main ----------------------------------------------------------------------------------------

int main(void) {
    stackmon_paint();
    glitchdet_boot();                   // why we last reset, before anything else changes it
    stdio_init_all();
    console_gate_set(true);             // the console is locked until shell/login.c says otherwise

    // Give a USB serial console a moment to attach so boot messages aren't lost.
    for (int i = 0; i < 20 && !stdio_usb_connected(); i++)
        sleep_ms(100);
    log_init(&log_hooks);
    log_msg(SOS_LOG_NOTICE, "SOS booting");

    // Covers the rest of boot too: a hang there resets and retries instead of sitting dead.
    fault_boot();
    wdog_start();
    if (fault_last())
        log_msg(SOS_LOG_ERR, "last reset: %s", fault_last());
    else if (wdog_caused_reset())
        log_msg(SOS_LOG_WARNING, "last reset: watchdog timeout");

    // System clock from the RTC, before anything that might want the time.
    timesync_boot();

    tls_init();                         // wolfSSL heap accounting, before any allocation
    if (wolfCrypt_Init() != 0 || wolfSSH_Init() != WS_SUCCESS) {
        log_msg(SOS_LOG_ERR, "crypto init failed");
        return 1;
    }

    // The root secret, then configuration and host key: all three may write flash, which is
    // simplest before Wi-Fi. Nothing derives a key from the root secret yet.
    consoleonly_init();                 // before anything loads a record from flash
    rootsource_init();                  // OTP, the flash record, or nothing: starts the key provider
    failflash_init();                   // wrong console passwords so far, before the console starts
    settings_init();
    sensors_init();                     // a temperature, kept from before the detector is armed
    glitchdet_start(config_glitch_mode(settings()->glitch_detector));   // armed unless set otherwise
    timesync_apply_cal();               // the RTC's calibration is a setting
    const uint8_t *hostkey;
    uint32_t hostkey_len;
    hostkey_init();
    hostkey_get(&hostkey, &hostkey_len);    // now, before Wi-Fi; none yet on a board not set up
    cardkey_init();                     // the base of the key for encrypted SD cards

    if (cyw43_arch_init_with_country(wifi_country_code())) {
        log_msg(SOS_LOG_ERR, "cyw43 init failed");
        return 1;
    }
    wifi_start();                       // connects in the background; the console works meanwhile

    // Every key has been derived by now: no more until reset (core/keyprov.h).
    keyprov_lock();

    // The console: locked until its password is typed, or open straight away if none is set.
    shell_bind(&console_shell, &console_io, NULL);
    shell_console_start(&console_shell);
    chat_init(&chat_hooks);
    chat_register(&console_shell, 0, "console", "console");
    // From here on, log lines are printed by log_flush() in the loop, between commands.
    log_set_deferred(true);

    bool sshd_failed = false;
    absolute_time_t next_blink = get_absolute_time();
    bool led = false;
    for (;;) {
        wdog_feed();
        wifi_poll();
        // Serve SSH once there's a network and someone to let in (keys may be added later).
        if (!sshd_running() && !sshd_failed && wifi_up() && settings()->key_count) {
            // A board set up since boot gets its host key now: it is tied to the owner, who
            // didn't exist before.
            // Asked every time, not remembered here: `config reset` takes the host key away
            // with the owner it belonged to (hostkey_owner_gone).
            if (hostkey_get(&hostkey, &hostkey_len))
                sshd_failed = !sshd_start(hostkey, hostkey_len);
        }
        if (wifi_up())
            timesync_network_up();      // starts SNTP only if the RTC had no valid time
        sensors_poll();                 // before glitchdet_poll, which keeps the latest readings
        glitchdet_poll();
        timesync_poll();
        nts_poll();
        shell_poll(&console_shell);
        // The console locks itself when the USB terminal goes away, and when nothing was typed
        // for SOS_CONSOLE_IDLE_LOCK_MIN minutes. (The UART has no connection signal: for it
        // there is only the idle lock.)
        login_poll(&console_shell, stdio_usb_connected(), SOS_CONSOLE_IDLE_LOCK_MIN * 60000u);
        console_gate_set(login_locked());   // follows a login, a logout or a lock in this pass
        if (!login_locked())            // a locked console shows nothing
            chat_deliver(&console_shell);   // wall and chat messages from SSH sessions
        sshd_poll();
        lua_host_poll(true);            // requests from a Lua script on core 1
        log_flush();                    // messages logged in interrupt context
        if (time_reached(next_blink)) {
            led = !led;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led);
            next_blink = make_timeout_time_ms(led ? 50 : 1950);   // brief blink every 2 s
        }
        sleep_ms(5);
    }
}
