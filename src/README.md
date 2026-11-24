# Source layout

SOS is one firmware image with one main loop. Each directory holds one subsystem and has a
README on how it works. Headers are included by subsystem path, e.g.
`#include "time/nts.h"`, with `src/` on the include path. So every dependency between
subsystems is visible at the top of the file.

| Directory | Subsystem |
|---|---|
| [`core/`](core/README.md) | System log, configuration (format and flash storage), watchdog, stack monitor |
| [`shell/`](shell/README.md) | Command shell: line editing, questions, long-running jobs, the commands |
| [`net/`](net/README.md) | Wi-Fi station connection, HTTPS client |
| [`ssh/`](ssh/README.md) | SSH server (shell sessions and SFTP), host key; `pico-sshd/` is the adapted upstream server |
| [`files/`](files/README.md) | SD card, FatFs file commands, path handling and the write check |
| [`time/`](time/README.md) | RTC, system clock, NTS (authenticated NTP) and SNTP |
| [`tls/`](tls/README.md) | TLS 1.3 client and its trusted root certificates |
| [`lua/`](lua/README.md) | Lua scripts on core 1 |

`main.c` sets everything up and runs the main loop.

## The main loop

There are no threads and no RTOS: everything on core 0 runs from one loop in `main.c`. Each
subsystem has a `*_poll()` function that does a bounded amount of work and returns. Nothing may
block: a step that waits (a TCP connect, a TLS handshake, a question on the console) is a state
machine that moves on when the loop comes round again. The watchdog is fed once per pass, so a
pass that takes longer than `SOS_WATCHDOG_MS` (8 s) resets the chip.

```
boot:  stack paint, stdio, log, watchdog, clock from RTC, wolfSSL/wolfSSH,
       configuration and host key (both may write flash), CYW43, Wi-Fi, console shell

loop:  wdog_feed        wifi_poll          sshd_start (once Wi-Fi and a trusted key exist)
       timesync_poll    nts_poll           shell_poll (console)    sshd_poll (SSH sessions)
       lua_host_poll    log_flush          LED blink               sleep 5 ms
```

## Interrupt context

lwIP and the CYW43 driver run in interrupt context (`pico_cyw43_arch_lwip_threadsafe_background`).
Their callbacks only buffer data and set flags. The `*_poll()` functions do the real work in
the main loop, and wrap lwIP calls in `cyw43_arch_lwip_begin()`/`end()`. Interrupt handlers
share the core 0 stack (8 KB, at the top of RAM). The log stores messages from interrupt
context, and `log_flush()` prints them later.

## Core 1

Core 1 runs only Lua scripts (see [`lua/`](lua/README.md)). It never touches the card, the
network, the log or the shells; it asks core 0 through a mailbox. Writing flash pauses core 1
(`flash_safe_execute`).

## Trust boundary

The USB/UART console is the only *admin* channel (`shell_io_t.admin`). Only the console can
change the configuration, write `/certs`, format the card or approve root certificates. SSH
sessions can view the configuration and use files, but can't change any of that. The console is
also needed for what affects everyone or what TLS trusts: log levels, `time sync sntp`,
`nts ke <host>` and `sd test`. Physical
access is the trust boundary, since it already allows reflashing.

## Tests

Modules marked *pure C* in their READMEs have no Pico SDK dependency and are unit tested on the
host (`tests/`, see the top-level README). Platform access goes through small interfaces
(`config_store_t`, `log_hooks_t`, `shell_io_t`), so the logic runs without the hardware.
