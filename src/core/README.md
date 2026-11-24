# core: log, configuration, watchdog, stack monitor

The services every other subsystem uses.

## Log (`log.c`)

`log_msg(level, fmt, ...)` stores each message in a 4 KB RAM ring, which `dmesg` shows in any
shell. If the message is important enough it's also printed on the console. Levels are the
syslog ones (`SOS_LOG_ERR` … `SOS_LOG_DEBUG`), with separate thresholds for the console
(default notice) and the history (default info). Lines are cut at 160 characters.

- **Input-line redraw:** before printing, the log hides the console's input line, and redraws
  it afterwards (`log_hooks_t.hide_input`/`show_input`), so a message never mangles what's
  being typed.
- **Interrupt context:** a message logged from an interrupt (lwIP, CYW43) is only stored;
  `log_flush()` in the main loop prints it.
- **Deferred mode**, turned on once the main loop runs: every message waits for
  `log_flush()`, so log lines never land in the middle of a command's output.
- **Pure C:** all platform access goes through `log_hooks_t`, so it's unit tested on the host.

## Configuration

Two layers:

- **`config.c`, the record format (pure C, host tested).** `sos_config_t` holds the Wi-Fi
  network, hostname, SSH user, up to 8 trusted keys, the NTS settings and the approved
  root-certificate hash. It also has the checks and parsers for each field, such as
  `config_key_parse()` for an `authorized_keys` line.
  - **A/B slots:** a record goes into one of two flash slots, with a sequence number and a CRC.
    Each save goes to the slot that doesn't hold the current record, so a power cut mid-save
    leaves the previous version.
  - **Append-only format:** new fields only go at the end, and the stored length says which
    version wrote the record. `record_lengths` lists every released size (1540, 1606, 1638).
    A record from older firmware loads with the newer fields at their defaults; firmware that
    doesn't know a record's length ignores it. `_Static_assert`s pin the offsets of released
    fields.
  - **Storage interface:** flash is reached through `config_store_t`, which the tests replace
    with a fake flash that can fail or lose power mid-write.
- **`settings.c`, the live configuration on the device.** `settings()` returns it; edit it,
  then call `settings_save()`.
  - **Where it lives:** the two slots are the flash sectors just below the SSH host key:
    `… firmware … | slot A | slot B | host key |` (end of flash).
  - **First boot:** with nothing saved, the configuration is seeded from the build-time
    defaults in `local.cmake` (the generated `sos_defaults.h`) and saved.
  - **Reset:** `config reset` goes back to those defaults except the trusted key: it saves a
    configuration without keys, so a key that was removed doesn't come back.
  - **Flash writes** go through `flash_safe_execute()`, which pauses core 1 (Lua) while
    flash is busy.

Only the console may change the configuration; see `require_admin()` in `shell/shell.c`.

## Watchdog (`wdog.c`)

`wdog_start()` runs early in `main()`: it records whether the last reset was a watchdog
timeout, then starts the hardware watchdog. The main loop calls `wdog_feed()` on every pass.
If a pass takes longer than `SOS_WATCHDOG_MS` (default 8000; `0` disables it), the chip
resets. After such a reset, `uptime` and the boot log say so.

## Fault record (`fault.c`)

A hard fault or an SDK panic stores a record in the watchdog's scratch registers 0-3 (they
survive a watchdog reset; the SDK uses 4-7) and resets the chip at once with
`watchdog_reboot()`. `fault_boot()`, early in `main()`, reads and clears it; the boot log and
`uptime` show it.

- **Hard fault:** the record holds the core, the program counter and link register from the
  exception frame, and the fault status (CFSR). The handler switches to its own small stack
  and lifts the stack limit first, so it also works after a stack overflow (when there is no
  usable frame: the record then has no addresses). It is installed in the RAM vector table by
  `fault_boot()`, because the SD card library defines `isr_hardfault` itself (its handler stops
  at a breakpoint, which without a debugger hangs until the watchdog). Both cores share the
  table, so a fault in Lua on core 1 resets the device too, instead of leaving core 1 dead.
- **Panic:** `PICO_PANIC_FUNCTION=sos_panic` (CMake). The record holds the caller's address and
  the address of the message, which the next boot prints if it is the same build.
- `crash fault|stack|panic|hang` (console only) calls `fault_test()`.

## Stack monitor (`stackmon.c`)

`stackmon_paint()`, the first call in `main()`, fills the unused part of the core 0 stack with
a pattern. `stackmon_peak()` finds the deepest point that has been overwritten since, and `mem`
shows it. Interrupt handlers use the same 8 KB stack.
