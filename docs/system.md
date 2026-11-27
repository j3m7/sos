# Logging, watchdog, temperature and memory

## Logging

Boot, Wi-Fi, configuration and SSH server messages go to the system log:

- The serial console prints them as they happen. A message arriving while you type clears your
  input line, prints, and redraws the prompt and what you typed (a hidden password isn't
  redrawn).
- `dmesg` shows the last 4 KB of messages in any shell, SSH sessions included; `dmesg -f
  <facility>` only one facility's.
- Each message has a facility, which says which log it belongs to (#104): `kern` (boot, resets,
  faults, glitch detector, root secret), `auth` (console and SSH logins, passwords, and every
  configuration change), `daemon` (Wi-Fi, SSH server, time, HTTPS, SD card, chat) or `user`
  (Lua scripts and `logger`).
- **Log files on the SD card:** every message the history keeps is also written to the card,
  to one file per facility: `/var/log/kern.log`, `auth.log`, `daemon.log`, `user.log`, and
  `syslog` for messages without a facility (the last resort). Lines read
  `2026-11-26T14:03:12.345Z notice console: logged in` (UTC; `+12.345`, seconds since boot,
  while the clock isn't set), and each boot starts a file with a `--- boot: SOS <commit>, last
  reset: <why> ---` line. Warnings and errors are written at once, the rest within 5 s. The card
  is mounted for this at boot; without one, messages wait in the 4 KB history, a mount is tried
  quietly every minute (not after `sd unmount`, until something mounts the card again), and
  once one is in they're written, with a line saying how many were dropped meanwhile.
  Encrypted or plain, whichever card is in. A failed write stops the files until the card is
  mounted again. `log` shows the state; `log file <level>` sets what's written (default `info`).
- **Rotation:** every 7 days at 00:00 UTC by default, each file becomes `<name>.1` (replacing
  the previous one) and a new one starts, so the card holds the current period and the one
  before. `log rotate` rotates now; `log rotate every <N>d` and `log rotate at <HH:MM>` change
  when (saved in the configuration; console only). The time of the last rotation is kept on
  the card (`/var/log/.rotated`), so a reboot keeps the count and a rotation missed while the
  board was off happens at the first chance; without the clock set nothing rotates.
- **Protected:** like `/certs`, `/var/log` can be read from anywhere (SSH, SFTP, Lua) but
  changed only on the console, so a session that gets in can't erase its traces. `/var`
  itself is protected too (renaming it would move `/var/log`); the rest of `/var` isn't.
- `logger [-p <level>] <message>` adds a line to the log from any shell (facility `user`, at
  `notice` unless `-p` says otherwise), tagged with the shell it came from. At most 10 a second.
- `log console <level>` sets what the console prints (default `notice`); `log history <level>`
  what's kept for `dmesg` (default `info`). Levels: err, warning, notice, info, debug. Console
  only to change; `log` shows the levels in any shell.
- Only printable ASCII is logged: other bytes in quoted text (a user name, a file name) become
  spaces or `?`.

## Watchdog

SOS enables the RP2350 hardware watchdog right after the boot banner. Everything runs from the
main loop, which feeds it on every pass; if the loop stops coming round for `SOS_WATCHDOG_MS`
(default 8000 ms), the chip resets. A hang during boot therefore resets and retries too.
Nothing blocks the loop for long: Wi-Fi connects in the background and console questions
(`setup`, passwords) wait for input without stopping it.

- After a watchdog reset the boot log shows `last reset: watchdog timeout` and `uptime`
  appends `(last reset: watchdog)`. Power-on and the `reboot` command don't count.
- A hard fault (bad memory access, undefined instruction, stack overflow, on either core) or
  an SDK panic doesn't wait for the watchdog: it stores what happened and resets at once. The
  boot log and `uptime` then show it, e.g. `last reset: hard fault on core 0 at 0x1000a3c4,
  called from 0x10004211 (bad memory access, CFSR 0x00008200)`. Look the addresses up in
  `build/sos.dis`. The console command `crash fault|stack|panic|hang` stops the firmware in
  each way, for testing this.
- `ssh` shows the longest single SSH work step (`longest ... ms`), a guide to how much margin
  the timeout has.
- Set the timeout when configuring, e.g. `cmake --preset debug -DSOS_WATCHDOG_MS=12000`; the
  RP2350 maximum is 16777 ms and `0` disables the watchdog. It pauses while a debugger halts
  the core.

## Health checks

SOS checks itself at boot and every minute while it runs. At boot, before anything can make a
key: the last reset, the clocks, the flash chip, the random number generator's raw output (NIST
SP 800-90B's startup tests) and the crypto (known answers for SHA-2, HMAC, AES-GCM, AES-XTS,
Ed25519 and X25519). Every minute: the glitch detector's guard, both cores' stacks, free heap,
the chip's temperature and the supply. `health` shows the
latest result of each (PASS, WARN, FAIL or SKIP, with the reason), in any shell; `health run`
and `health full` run them now, on the console only. A change of status is logged (`kern`): a
healthy board logs only `health: boot checks ... in N ms`. A failed check refuses what depends
on it (making keys, encrypting new data), never what only reads what is stored. Lua scripts read
the results with `sos.health()`. See `man health` and the design,
[`docs/design/health.md`](design/health.md).

## Chip temperature calibration

`temp` reads the RP2350's own temperature sensor. Uncalibrated it is a few degrees off: the
sensor differs from chip to chip, and the conversion assumes the 3.3 V supply is exactly 3.3 V.
That's fine for watching a change on one board. For an absolute figure, or to compare boards,
calibrate it against a thermometer. `temp cal` (console only) stores the difference, at most
15 degrees either way, and adds it to every reading: `temp`, the log and the glitch
detector's reports.

1. **Let the board cool.** Leave it unpowered for at least half an hour beside the reference
   thermometer, out of drafts and sun, so that the chip and the thermometer are at the same
   temperature.
2. **Power it up and log in.** Wait until the log shows `wifi: connected` (readings shift a
   little when Wi-Fi starts, and the stored difference should match the readings after that),
   but no longer than a minute: the chip warms itself.
3. **Read the thermometer and set it:** `temp cal 23.5`, with your reading. You should see
   `the sensor reads 21.7 C and the reference 23.5 C: +1.8 C is now added to every reading`.
   If it says the reference is more than 15 degrees from the sensor, stop: check the
   thermometer, and that the board had cooled.
4. **Check:** `temp` now ends with `Calibrated (`temp cal`): +1.8 C added to the sensor's
   reading.`

`temp cal` alone shows the calibration; `temp cal off` removes it. A new `temp cal` replaces
the old one (it measures against the sensor's own reading, not the calibrated one), and
`config reset` removes it (`temp_cal` in the provisioning file carries a known value back).
It corrects the offset only, so it's best near the temperature it was set at.

What it stores equates the sensor's reading with the room thermometer at that moment, and by
then the chip is already a little warmer than the room from running. So that warmth is folded
into the calibration: a calibrated board reports something closer to the room temperature as
worked out from the chip than the chip's own temperature, and under load the chip itself is
warmer than it says. Don't compare a calibrated reading with the chip's rated maximum.

The calibration is stored in two bytes of the configuration that were spare, so firmware
without `temp cal` still reads the configuration (and ignores it).

## Memory layout

| Region | Use |
|---|---|
| Main RAM (512 KB) | data, heap, and the core 0 stack (top 8 KB, `SOS_CORE0_STACK_SIZE`) |
| SCRATCH_X (4 KB) | core 1 stack (`PICO_CORE1_STACK_SIZE`), as in the SDK default; while Lua runs, core 1 uses a 32 KB stack from the heap instead |
| SCRATCH_Y (4 KB) | free |
| Flash, last 4 KB sector | keys: SSH host key (page 0); page 1 held the SD cards' device key of older firmware and is left as it is |
| Flash, 2 sectors below it | configuration (A/B records) |
| Flash, the sector below those | the board's root secret (open boards; nothing is stored under it yet), see [`src/core/README.md`](../src/core/README.md) |

The core 0 stack is moved out of SCRATCH_Y by the linker script overrides in `config/ld/`.
Interrupt handlers, including lwIP/CYW43 network processing, share it. The `mem` shell command
reports the stack high-water mark; an SSH login peaks at about 3.2 KB. It also shows how deep the
last Lua script went into core 1's stack.
