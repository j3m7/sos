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
  - **The console password** is stored as a verifier (`console_iterations`, `console_salt`,
    `console_verifier`; `pwverify.c`, pure C over wolfCrypt, host tested): PBKDF2-HMAC-SHA256
    of the password, then HMAC-SHA256 under `KEYPROV_CONSOLE_PASSWORD`. Like the owner secret
    it exists only in sealed records. The other way round, `console_password` exists only in a
    plain record: the provisioning tool puts the password to set there, `settings.c` turns it
    into the verifier before sealing, and it's never written sealed.
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

## The root secret in OTP, and choosing its source (`roototp.c`, `rootsource.c`)

On a secured board the root secret is in one-time-programmable memory instead of flash. This
firmware reads it there; a separate image, used once per board, writes it.

- **Where:** OTP pages 4, 5 and 6, tried in order (page 3 holds SOS's own flags). In a page:
  the 32-byte secret in rows 0 to 15, a 4-byte check over it in rows 16 and 17
  (HMAC-SHA256 keyed with the secret), the marker 0xa55a in row 18, written last, and the
  complement of the secret's raw rows in rows 32 to 47 against imaging the chip.
- **What a page is:** blank; complete (the marker row reads 0xa55a); spent (something written,
  the marker row still blank: a burn that stopped, skipped for good); damaged (the marker row
  holds something else); or unreadable. The first page that isn't spent decides, and a
  damaged or unreadable one means no secret, not "try the next".
- **Locks:** a spent page holds 32 bytes and no marker. If the USB bootloader could write it,
  someone with a cable could add the check and the marker and make it the board's root
  secret, one they know. So every list page up to the secret's must be locked against the
  bootloader's writes (the burn image does it). The firmware reads those lock words at boot:
  an open board warns, a sealed-only board refuses the secret.
- **Reading it:** what a page holds is decided with reads that can't fault. The secret itself
  is read through the chip's guarded alias, which faults instead of returning a wrong value,
  and then verified against its check. A complete page that fails its check is not a secret
  and nothing replaces it.
- **Which source** (`roototp_choose`, in `rootsource_init()` at boot): OTP if a secret is
  there; otherwise **nothing on a sealed-only board**, whose flash is never read for a root
  secret nor given one (anyone with a cable could write a root secret there with a
  configuration sealed under it); otherwise the flash record of an open board, as before. An
  open board whose OTP pages can't be read or hold a damaged secret also gets nothing, said
  plainly, instead of quietly making a new flash secret.
- **Locked after boot:** with the secret in OTP, `keyprov_lock()` sets the chip's soft lock for
  the page (a register, until the next reset; not a write to OTP) and checks that the page
  then reads as unreadable.
- `roototp.c` has no hardware in it and is unit tested on the host. No board has a secret in
  OTP yet: on the board only the paths with blank pages have run.

## The burn image (`rootburn.c`, `rootburn_board.c`)

Writing a board's root secret into OTP is permanent and is not something everyday firmware can
do: the code is compiled only with `-DSOS_BURN_IMAGE=ON`, into an image flashed for that one
job (`docs/securing-a-board.md`, step 3). `rootburn_board.c` refuses to compile otherwise.

- **`rootsecret burn`** (console only): asks for the board's serial, then a typed word, then
  waits for the BOOTSEL button to be held. `rootsecret burn plan` says which page it would
  use and writes nothing. `rootsecret burn rehearse` stops on purpose before the secret is
  complete, spending one page, to try the interrupted case.
- **The order** (`rootburn_run`): lock earlier spent pages; take 32 random bytes; write them a
  row at a time with read-back; write the chaff; write the check; read everything back and
  verify; write the marker last; lock the page. A failure anywhere leaves a spent page with
  no marker, locked, and the next run uses the next page.
- **The only place OTP is programmed** is `program()` in `rootburn_board.c`, and it refuses
  any row outside the root secret's three pages and their lock words.
- `rootburn.c` has no hardware in it and is tested on the host against a model of OTP whose
  bits can only be set: every single write failing, every write silently not taking, and
  power lost after every write. **It has never been run on a chip.**

## Wrong-password count (`failcount.c`, `failflash.c`)

The console delays the next password check after each wrong answer (`shell/login.c`). The
count behind that delay is kept in flash so that a reboot doesn't reset it.

- **Counted before, cleared after:** `failcount_attempt()` runs before a check starts and
  `failcount_clear()` after a right answer. A check that never finished (power pulled, reset,
  Ctrl-C) therefore stays counted.
- **Its own sector,** the fifth from the end, below the root secret's. Flashing, provisioning,
  `config reset` and `rootsecret forget` don't touch it.
- **No erase per answer:** flash bits can be cleared one at a time. One track has a bit cleared
  per check started; a second track is cleared up to the same point by a right answer; the
  count is the difference. The sector is erased once when first used, and again only when
  the first track is full, after 14336 checks; what is outstanding is carried over (capped
  at 64, far beyond the longest delay). A power cut during that one erase loses the count.
- **If flash can't be written** the count goes up in RAM until the next reboot, and the log
  says so once.
- **Limit:** someone with a cable and the USB bootloader can erase the sector. `failcount.c`
  has no hardware in it and is unit tested on the host.

## Key sector (`keysector.c`)

The last 4 KB flash sector holds keys, one 256-byte flash page each: page 0 the SSH host key
(`ssh/hostkey.c`). Page 1 held the SD cards' device key of firmware from before the owner
secret; nothing reads or writes it now, and it's left as it is. The sector is above the
configuration sectors, so `picotool load`, `config reset` and provisioning don't touch it.
`keysector_write()` replaces one page and keeps the others; it erases and rewrites the whole
sector, so a power cut during a write can lose what's in it. Writes happen only when the host
key is made or sealed.

## Reading OTP and the boot key check (`otpread.c`, `bootkey.c`)

The RP2350's one-time-programmable memory holds the flags that secure a board (secure boot,
debug off) and the hashes of the keys its firmware must be signed with.

- **`otpread.c` only reads,** through the chip's memory-mapped read aliases, to which a write
  is a bus fault. It never calls the boot ROM's `otp_access`, the function that can program
  OTP: everyday firmware carries no code that writes it. Flag rows are stored several times
  and read by vote (three of eight for `CRIT1`, two of three for the boot flags).
- **Unreadable is not a state:** a row the code may not read comes back as all ones, which
  would look like every flag set. It is reported as unreadable. The secure boot and debug
  flags are taken from the chip's `CRITICAL` register (what it latched at reset and acts on),
  with the `CRIT1` rows as a cross-check.
- **`bootkey.c`** (no hardware, unit tested) finds the public key in the running image's
  signature block (the block format of datasheet section 5.9; the last image definition in
  the block loop is the one the boot ROM uses) and compares its SHA-256 with the boot key
  slots. A slot counts when it is marked valid and not marked invalid.
- **Shown by** `version` (secure boot, debug port) and `bootkey` (the verdict). The procedure
  in `docs/securing-a-board.md` runs `bootkey` before the bit that turns secure boot on.
- **Not for secrets:** these are the unguarded read aliases, which return all ones for a row
  that can't be read. Reading the root secret from OTP will need the guarded ones.

## Sealed-only boards (`sealedonly.c`, `sealedonly_board.c`)

An open board loads an unsealed configuration or host key from flash and seals it; that is how
`ninja provision` works. A sealed-only board refuses them: anyone with a cable can write the
flash, and an unsealed configuration naming their own SSH key would otherwise let them in.

- **The marker** is three OTP rows, 0x0c0 to 0x0c2 (the start of page 3, the first page the
  chip leaves to the user), each holding the raw value 0x5ea1ed, read by a two-of-three vote.
  It is not secure boot: a development board can have secure boot and stay provisionable.
- **It fails closed.** Blank rows mean no. The marker means yes. Rows that can't be read, or
  that are neither blank nor the marker, also mean yes, and the log and `version` say which.
  OTP bits can be set but not cleared, so the marker can be damaged but not removed.
- **What it changes:** `core/settings.c` and `ssh/hostkey.c` refuse unsealed records
  (`refuse_plain`); `rootsecret forget` is refused. An unsealed record that was refused is
  reported in the log and left in flash. If it replaced the sealed one, the board has no
  configuration and is set up on its console: an attacker with a cable can wipe a sealed-only
  board, as they can erase its flash, but can't give it a configuration.
- **The firmware only reads the rows.** Writing them is a step of `docs/securing-a-board.md`,
  for production boards, done by the owner with `picotool`.
- **`-DSOS_FORCE_SEALED_ONLY=ON`** builds an image that treats every board as sealed-only, to
  try the behaviour on a development board without touching its OTP. It can only add the
  restriction. `sealedonly_decide()` has no hardware in it and is unit tested on the host.

## Glitch detector (`glitchdet.c`)

The RP2350 has four detector circuits that reset the chip when its clock or core supply is
disturbed (datasheet section 10.9). A secured board would arm them for good with an OTP bit.
`glitchdet.c` arms them by software instead, from a setting, to find out in normal use
whether they ever reset a healthy board (`docs/securing-a-board.md`).

- **On by default:** armed at sensitivity 3, the highest, unless the setting says otherwise
  (`glitch off`, `glitch on <0-3>`, `glitch watch <0-3>`; `glitch default` goes back). A
  detector reset that nobody forced is a finding to report: see "Glitch detector resets" in
  [`docs/development.md`](../../docs/development.md).
- **Two modes:** *watching* (`glitch watch <0-3>`) runs the detectors at a sensitivity
  without arming them, and logs a trigger without resetting anything; *armed* (`glitch on`)
  resets the board on a trigger, as the OTP flag would.
- **Not protection:** it is armed after the configuration loads, software can disarm it, and
  the guard below disarms it after repeated resets, which is what deliberate glitching causes.
- **What survives a detector reset** is kept in POWMAN scratch registers 0 to 2 (that reset
  clears the watchdog's scratch registers, and so the fault record): the number of detector
  resets since power-on, how many came in a row soon after boot, and the uptime.
- **Reported at boot** (log, `uptime`, `glitch`): that the last reset was the detector's, which
  detectors fired, after how long.
- **The guard:** three detector resets in a row, each within three minutes of starting, and
  the next boot doesn't arm it, so the board stays reachable to turn the setting off.

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
