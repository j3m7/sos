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
  - **Sealed or plain:** the header's version says which. Version 2 is sealed: after the same
    16-byte header come a nonce, the ciphertext and a tag, with the header (magic, version,
    length, sequence number) as associated data, so a sealed payload can't be put under another
    sequence number. Version 1 is plain, as older firmware and the provisioning tool write it.
    `config.c` seals through `sealer_t` (`sealer.h`) and has no cryptography in it: the device
    plugs in AES-256-GCM (`seal.c`), the tests a fake, the provisioning tool nothing.
  - **A plain record on a board that seals** loads, and the next save (which `settings.c` does
    at once) stores it sealed and erases the slot that held it, checking that it reads blank.
    If power is lost in between, the next boot does it again.
  - **Plain before sealed:** a plain record comes before a sealed one whatever their sequence
    numbers. A plain record next to a sealed one was written after the sealing, by older
    firmware after a downgrade (which numbers from 1 again) or by provisioning, so it's the
    configuration the user set last. The sealed save that follows is numbered above both.
    `config_scrub_plain()` erases only what has a plain record's header and doesn't load
    (damaged), next to the sealed configuration.
  - **A secured board** (`refuse_plain`, not set anywhere yet) doesn't see plain records at
    all: they take no part in choosing the newest record, so a planted one can't hide the sealed
    configuration.
  - **The owner secret** (`owner_secret`, the last field) ties the board's encrypted SD cards
    to this configuration (`files/cardkey.h`). It exists only in sealed records: `config.c`
    zeroes it when it loads a plain record and never writes it in one, so a configuration that
    arrives plain (provisioning, older firmware) can't carry or plant one. `settings.c` makes a
    new one whenever it saves a configuration that has none. `config reset` destroys it.
  - **A sealed record that doesn't open** (another root secret, or changed) counts as no
    record. It isn't erased automatically; the log says it's there
    (`config_sealed_unopened()`). Later saves take its slot in their turn.
  - **Storage interface:** flash is reached through `config_store_t`, which the tests replace
    with a fake flash that can fail or lose power mid-write.
- **`settings.c`, the live configuration on the device.** `settings()` returns it; edit it,
  then call `settings_save()`.
  - **Where it lives:** the two slots are the flash sectors just below the SSH host key:
    `… firmware … | root secret | slot A | slot B | key sector |` (end of flash).
  - **Nothing saved** (a new board, after `config reset`, or records this firmware can't
    read): the configuration is `config_defaults()`, with no network and no trusted keys. The
    firmware carries no configuration of its own.
  - **Provisioning:** `tools/provision.sh` (`ninja provision`) builds a record on the host
    with `config.c` itself (`tools/host/mkconfig.c`) and loads it into slot A with `picotool`,
    erasing slot B. The device validates it like any saved record when it boots, and seals it.
  - **The seal key** (`KEYPROV_CONFIG_SEAL`) is derived in `settings_init()`, before the key
    provider locks, and kept in RAM by `settings.c` while the board runs: every save needs it.
    Without a usable root secret a sealed configuration can't be read and nothing can be
    saved; the log says so.
  - **Flash writes** go through `flash_safe_execute()`, which pauses core 1 (Lua) while
    flash is busy.

Only the console may change the configuration; see `require_admin()` in `shell/shell.c`.

## Root secret and key provider (`rootsecret.c`, `rootflash.c`, `keyprov.c`)

The base for protecting what the board stores (issue #64). The saved configuration and the SSH
host key are sealed under keys from it (`seal.c`: AES-256-GCM, a random nonce per seal, the
record's header as associated data). The key for encrypted SD cards comes from it too, together
with the configuration's owner secret (`files/cardkey.c`).

- **Root secret:** 32 random bytes, made from the TRNG at the first boot. Every other key is
  derived from it. On an open board (no secure boot) it's kept in flash, which protects nothing
  from someone who has the board; it's there so the same code and formats run as on a secured
  board, where it will be in OTP memory.
  - **Its own sector,** the fourth from the end of flash, below configuration slot A
    (`rootflash.c`). Not a page of the key sector: firmware from before the root secret rewrites
    that sector with only the pages it knows whenever it makes a host key, which would erase it.
    Flashing, provisioning and `config reset` don't touch the sector.
  - **The record** (`rootsecret.c`, pure C, host tested): magic `SOSR`, version 1, the secret,
    an FNV-1a check. The sector holds it twice, in its first two flash pages, so one damaged
    page doesn't lose everything derived from the secret; either valid copy is used, and a
    damaged one is reported (`rootsecret`, the log).
  - **Making it** programs page 1 and then page 0, without erasing: blank pages need no erase,
    and the rest of the sector is never touched. A secret is used only once page 0 holds it, so
    a first write cut off by a power loss (page 0 still blank) is finished or started over at
    the next boot, since nothing can have been stored under it.
  - **Never replaced automatically** otherwise: when page 0 isn't blank and there's no valid
    copy (damaged, or a record from newer firmware), or the two copies hold different secrets,
    the board has no keys, the log and `rootsecret` say so, and `rootsecret forget` on the
    console erases the sector. Only `rootflash.c` can read the secret out
    (`rootsecret_priv.h`).
- **Key provider** (`keyprov.c`, pure C over wolfCrypt, host tested):
  `keyprov_derive(purpose, out)` gives the 32-byte key for a purpose, HKDF-SHA256 of the root
  secret with a fixed label per purpose as the info and no salt. Callers never see the root
  secret, and can't supply any part of a label.
  - **Derive at boot, then lock:** `main.c` calls `keyprov_lock()` once boot is done, after
    which every derivation fails until reset. On an open board that's a flag; on a secured board
    the backend will also lock the OTP page. So a key can't depend on anything known only at
    run time: such a key is made by its user in a second step from a base key derived here.
  - **The labels are part of the stored formats.** The tests pin each purpose's key for a known
    root secret; changing a label would lock everything sealed under it.

## Key sector (`keysector.c`)

The last 4 KB flash sector holds keys, one 256-byte flash page each: page 0 the SSH host key
(`ssh/hostkey.c`). Page 1 held the SD cards' device key of firmware from before the owner
secret; nothing reads or writes it now, and it's left as it is. The sector is above the
configuration sectors, so `picotool load`, `config reset` and provisioning don't touch it.
`keysector_write()` replaces one page and keeps the others; it erases and rewrites the whole
sector, so a power cut during a write can lose what's in it. Writes happen only when the host
key is made or sealed.

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
