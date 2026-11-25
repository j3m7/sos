# core: log, configuration, watchdog, stack monitor, heap

The services every other subsystem uses.

## Log (`log.c`)

`log_msg(level, fmt, ...)` stores each message in a 4 KB RAM ring, which `dmesg` shows in any
shell. If the message is important enough it's also printed on the console. Levels are the
syslog ones (`SOS_LOG_ERR` … `SOS_LOG_DEBUG`), with separate thresholds for the console
(default notice) and the history (default info). Lines are cut at 160 characters.

- **Input-line redraw:** before printing, the log hides the console's input line, and redraws
  it afterwards (`log_hooks_t.hide_input`/`show_input`), so a message never mangles what's
  being typed.
- **Held console:** while a full-screen program (the editor) runs on the console,
  `log_hooks_t.hold` returns true and `log_flush()` prints nothing. Messages wait in the history
  and are printed once it ends; if they overflowed the history meanwhile, the console says some
  were dropped. `log_pending()` counts the messages waiting, so the program can say so.
- **Interrupt context:** a message logged from an interrupt (lwIP, CYW43) is only stored;
  `log_flush()` in the main loop prints it.
- **Deferred mode**, turned on once the main loop runs: every message waits for
  `log_flush()`, so log lines never land in the middle of a command's output.
- **Pure C:** all platform access goes through `log_hooks_t`, so it's unit tested on the host.

## Heap (`heap.c`)

`heap_info()` gives the heap's size, what's allocated and malloc's high-water mark (`mem` shows
them); `heap_free()` what's left, which TLS checks before opening a connection. The SDK wraps
malloc, calloc, realloc and free in a mutex so the two cores never allocate at once, but not
`mallinfo()`, which walks malloc's free lists. So `heap.c` also defines newlib's
`__malloc_lock()`/`__malloc_unlock()` (no-ops by default) with a recursive mutex: reading the
figures on core 0 can't race a Lua script allocating on core 1.

## Configuration

Two layers:

- **`config.c`, the record format (pure C, host tested).** `sos_config_t` holds the Wi-Fi
  network, hostname, up to 8 trusted keys, the NTS settings and the approved
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
  - **Nothing saved** (a new board, after `config reset`, or records this firmware can't
    read): the configuration is `config_defaults()`, with no network and no trusted keys. The
    firmware carries no configuration of its own.
  - **Provisioning:** `tools/provision.sh` (`ninja provision`) builds a record on the host
    with `config.c` itself (`tools/host/mkconfig.c`) and loads it into slot A with `picotool`,
    erasing slot B. The device validates it like any saved record when it boots.
  - **Flash writes** go through `flash_safe_execute()`, which pauses core 1 (Lua) while
    flash is busy.

Only the console may change the configuration; see `require_admin()` in `shell/shell.c`.

## Key sector and device key (`keysector.c`, `devkey.c`)

The last 4 KB flash sector holds the device's keys, one 256-byte flash page each: page 0 the SSH
host key (`ssh/hostkey.c`), page 1 the device key. It's above the configuration sectors, so
`picotool load`, `config reset` and provisioning don't touch it. `keysector_write()` replaces
one page and keeps the others; it erases and rewrites the whole sector, so a power cut during a
write can lose every key in it. Writes happen only when a key is created (the host key at first
boot, the device key at the first encrypted `sd format`) or cleared on request.

The **device key** (32 random bytes from the TRNG) wraps each encrypted SD card's data key
(`files/sdcrypt.h`). `devkey_create()` makes it only when the page holds no key record, so a
board that never encrypts a card never rewrites the sector for it. A page with the record's
magic is never overwritten automatically, even when it isn't valid: one flipped bit leaves the
key recoverable by hand, and a record from newer firmware must survive a downgrade. Such a
record leaves the device without a key (encrypted cards locked, `sd devkey` explains) until
`sd devkey forget` clears it.

**Downgrades:** firmware from before the device key writes the host key by erasing the whole
sector. It only does that when it has no valid host key, but then the device key goes too.

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
