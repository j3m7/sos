# core: log, configuration, watchdog, stack monitor, heap

The services every other subsystem uses.

## Log (`log.c`)

`log_msg(priority, fmt, ...)` stores each message in a 4 KB RAM ring, which `dmesg` shows in any
shell. If the message is important enough it's also printed on the console. Levels are the
syslog ones (`SOS_LOG_ERR` … `SOS_LOG_DEBUG`), with separate thresholds for the console
(default notice) and the history (default info). Lines are cut at 160 characters.

- **Facilities:** the priority is a facility OR-ed with a level, as in syslog:
  `log_msg(SOS_LOG_AUTH | SOS_LOG_NOTICE, ...)`. The facility is which log the message belongs
  to (#104), and each message belongs to exactly one:

  | Facility | What |
  |---|---|
  | `SOS_LOG_KERN` | boot, resets, faults, glitch detector, root secret / OTP, key sector, heap |
  | `SOS_LOG_AUTH` | console and SSH logins and connections, passwords, **every configuration change** (anything a person sets: `config:` messages, `log` levels, Wi-Fi keepalive), host key |
  | `SOS_LOG_DAEMON` | Wi-Fi, SSH server, NTS / SNTP / RTC, HTTPS / TLS, SD card, chat |
  | `SOS_LOG_USER` | Lua scripts, `logger` |
  | `SOS_LOG_NONE` (0) | not tagged yet: the last resort, ideally never used |

  Every caller names one; the text prefix (`wifi:`, `sd:`) names the subsystem and stays. The
  ring keeps it in each entry's first byte (`0x80 | facility | level`: text is printable ASCII,
  so the byte can't be mistaken for text or a newline), `log_history_next()` returns it, and
  `dmesg -f` filters on it. pico-sshd marks its login and connection messages with syslog's
  `LOG_AUTH`, which `ssh/sshd.c` maps to `SOS_LOG_AUTH`; the rest of its messages are daemon.
- **Rate limit for outside text:** `log_quota_t` lets at most N messages a second through and
  counts the rest, for sources a loop can drive: Lua scripts (`sos.log` and background output
  share one) and `logger`.

- **Input-line redraw:** before printing, the log hides the console's input line, and redraws
  it afterwards (`log_hooks_t.hide_input`/`show_input`), so a message never mangles what's
  being typed.
- **Held console:** while a full-screen program (the editor) runs on the console,
  `log_hooks_t.hold` returns true and `log_flush()` prints nothing. Messages wait in the history
  and are printed once it ends; if they overflowed the history meanwhile, the console says some
  were dropped. `log_pending()` says how many messages are waiting, so the program can say so. It
  reads a count per level kept as messages are added, dropped and shown: the lock is the
  interrupt disable, so it doesn't walk the history under it.
- **Interrupt context:** a message logged from an interrupt (lwIP, CYW43) is only stored;
  `log_flush()` in the main loop prints it. What is done in the interrupt is kept small: a
  message above the history level returns before it is formatted, and the others are formatted
  once. `log_vmsg_prefixed()` is for a caller that tags messages it passes on (the SSH
  server's go in as "sshd: ..."), so it needs no buffer or formatting pass of its own.
- **Deferred mode**, turned on once the main loop runs: every message waits for
  `log_flush()`, so log lines never land in the middle of a command's output.
- **Pure C:** all platform access goes through `log_hooks_t`, so it's unit tested on the host.

## Log files (`logfile.c`, `logfile_board.c`)

The log's messages also go to the SD card, one file per facility in `/var/log` (`kern.log`,
`auth.log`, `daemon.log`, `user.log`, and `syslog` for facility 0), each message to one file.

- **A third cursor:** `log.c` keeps `written` beside `start` and `printed`. `log_file_next()`
  hands the writer the next entry (priority, time since boot parsed back from the stored
  `[    1.234] ` prefix, and the text without the level's tag); `log_file_lost()` counts the
  entries the history dropped before the writer took them.
- **Buffers:** `logfile_poll()` (main loop) takes entries into a 1 KB buffer per file, while
  every buffer has room for one more line, so a busy file can't make another lose lines. A
  buffer is written when it's nearly full, holds a warning or an error, or its oldest line is
  5 s old; at most two files a pass. Each write opens the file for appending, writes and closes
  (closing syncs), making `/var/log` first if needed. Nothing stays open, so `sd unmount` and
  `sd format` are never blocked.
- **Times:** converted to UTC when written: the clock now, less the entry's age. Before the
  clock is set, `+<seconds since boot>`.
- **No card, a failed write:** with no card mounted the entries wait in the history and the
  drops are counted; the next mount writes them, with a `[log: N messages lost ...]` line in
  every file. A failed write stops the writer (one line in the history, which can't loop since
  it's stopped) until `sdcard_generation()` says the card was mounted again.
- **Mounting:** `logfile_start()` mounts the card once at boot, after the configuration (so an
  encrypted card opens). With no card mounted, `logfile_poll()` tries again every minute
  through `sdcard_mount_retry()`, which logs nothing on failure (the library's messages
  included), so a card put back is written to without anyone typing a command. It doesn't
  after `sd unmount` (the card may be on its way out) until something mounts it with
  `sdcard_mount()`. Any mount, by a file command, `sd`, SFTP or Lua, starts the files.
- **Level:** `logfile_set_level()` takes what's waiting under the old level first, so the
  record of `log file err` (logged just before) isn't filtered out by the change it records.
- **Rotation:** every `log_rotate_days` days (setting; default 7) at `log_rotate_minute` after
  midnight UTC (default 00:00), counted from the day of the last rotation. Each file becomes
  `<name>.1`, replacing the previous one, and the new file starts with a "rotated" marker.
  The time of the last rotation is kept on the card in `/var/log/.rotated` (Unix seconds),
  read once per mount: a reboot keeps the count, a rotation missed while the board was off
  happens at the first chance, and another card has its own. The first mount with the clock
  set starts the count without rotating; a kept time more than a day ahead (saved while the
  clock was wrong) starts it again. Nothing rotates by itself without the clock. A scheduled
  rotation happens before new messages are taken, so they go to the new files; `log rotate`
  (by hand) takes everything logged so far into the old ones first. A failed rename stops the
  files like a failed write.
- **Host tested** with the real `log.c` and a fake card (`tests/test_logfile.c`).

## Heap (`heap.c`)

`heap_info()` gives the heap's size, what's allocated and malloc's high-water mark (`mem` shows
them); `heap_free()` what's left, which TLS checks before opening a connection. The SDK wraps
malloc, calloc, realloc and free in a mutex so the two cores never allocate at once, but not
`mallinfo()`, which walks malloc's free lists. So `heap.c` also defines newlib's
`__malloc_lock()`/`__malloc_unlock()` (no-ops by default) with a recursive mutex: reading the
figures on core 0 can't race a Lua script allocating on core 1.

`heaptag.c` (pure C, host tested) says what the heap is used for. Features allocate with
`heap_alloc(tag, n)`, `heap_zalloc()`, `heap_realloc()` and `heap_release(tag, p)` instead of
malloc and free, and each tag (`ssh/tls`, `lua`, `editor`, `files`, `http`, `time`) keeps what it
holds now and its peak; `mem` lists them. Notes:

- **No per-block cost:** a block counts at its `malloc_usable_size()`, so releasing needs no size
  and no header. The price is that a block must be released under the tag it was allocated with.
- **`ssh/tls` is one tag:** wolfSSL's allocator hooks (`tls.c`) don't say who is asking, so SSH
  sessions, TLS connections and every other wolfCrypt allocation share it.
- **`other` in `mem`** is the heap in use minus the tags: code that calls malloc directly (the
  SDK, the SD card and Wi-Fi drivers) plus malloc's bookkeeping per block.
- **New code** should allocate through a tag, adding one to `heap_tag_t` if none fits.

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
    version wrote the record. `record_lengths` lists every released size (1540, 1606, 1638,
    ... 2184, and 2188 with the log rotation settings).
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
  - **Flash writes** go through `flashop_run()` (see "Flash writes" below), which pauses
    core 1 (Lua) and holds interrupts off while flash is busy.

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
  an open board warns, a console-only board refuses the secret.
- **Reading it:** what a page holds is decided with reads that can't fault. The secret itself
  is read through the chip's guarded alias, which faults instead of returning a wrong value,
  and then verified against its check. A complete page that fails its check is not a secret
  and nothing replaces it.
- **Which source** (`roototp_choose`, in `rootsource_init()` at boot): OTP if a secret is
  there; otherwise **nothing on a console-only board**, whose flash is never read for a root
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
job (`docs/security/design.md`, "Writing it"). `rootburn_board.c` refuses to compile otherwise.

- **`rootsecret burn`** (console only): asks for the board's serial, then a typed word, then
  waits for the BOOTSEL button to be held. `rootsecret burn plan` says which page it would
  use and writes nothing. `rootsecret burn rehearse` stops on purpose before the secret is
  complete, spending one page, to try the interrupted case.
- **The order** (`rootburn_run`): lock earlier spent pages; take 32 random bytes; write them a
  row at a time with read-back; write the chaff; write the check; read everything back and
  verify; write the marker last; lock the page. A failure after something was written leaves
  a spent page with no marker, locked, and the next run uses the next page.
- **A page that still reads blank is never locked.** If the first write fails and nothing
  landed, the page stays blank and unlocked and is the next run's page. A locked blank page
  could be neither written nor passed over, and would block all three pages for good.
  (`roototp.c` passes over a blank page the firmware can't write, should one ever exist.)
- **Stopping:** a run that ends in an error (other than nobody holding the button) makes the
  image refuse further runs until a restart, and says to find the cause first. A run on a
  board that already has its secret only locks pages an earlier run couldn't.
- **Nothing resets the board mid-burn:** the watchdog is fed before every write and the
  software glitch detector is off for the duration.
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

## Flash writes (`flashop.c`)

Every erase or program of the flash the firmware runs from goes through `flashop_run()`: the
configuration (`settings.c`), the wrong-password count (`failflash.c`), the root secret record
(`rootflash.c`) and the key sector (`keysector.c`). It wraps the SDK's `flash_safe_execute()`,
which stops core 1 and turns interrupts off for as long as the write takes. For that long
nothing else is served: not the Wi-Fi driver, not USB. It is the longest time this firmware
spends with interrupts off, so `flashop_run()` times it, and `uptime` shows how many writes
there were, how long the last held interrupts off, and the longest.

Measured on dev-1 (Pico 2 W, RP2350 A2), 2026-11-26, issue #109:

| Write | Interrupts off |
|---|---|
| Saving the configuration: one 4 KB sector erased, then nine pages programmed | 39 to 43 ms (three saves) |
| One page programmed, no erase (the wrong-password count at a console login) | 0.5 ms |

An SSH session open across three configuration saves kept working: 40 ms is far below TCP's
retransmission time. The erase is nearly all of it, and the flash part's worst-case erase time
is several times the typical one, so a save should stay something a person asks for, not
something done on a timer. If a write ever has to be shorter, erase and program can be made
separate calls so interrupts run in between.

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
- **Shown by** `version` (secure boot, debug port) and `bootkey` (the verdict). The runbooks
  (`docs/security/README.md`) run `bootkey` before the bit that turns secure boot on.
- **Not for secrets:** these are the unguarded read aliases, which return all ones for a row
  that can't be read. Reading the root secret from OTP will need the guarded ones.

## Console-only setup (`consoleonly.c`, `consoleonly_board.c`)

An open board loads an unsealed configuration or host key from flash and seals it; that is how
`ninja provision` works. A console-only board refuses them: anyone with a cable can write the
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
  configuration and is set up on its console: an attacker with a cable can wipe a console-only
  board, as they can erase its flash, but can't give it a configuration.
- **The firmware only reads the rows.** Writing them is step 4.6a of
  `docs/security/production-runbook.md`, for production boards, done by hand with `picotool`.
- **`-DSOS_FORCE_CONSOLE_ONLY=ON`** builds an image that treats every board as console-only, to
  try the behaviour on a development board without touching its OTP. It can only add the
  restriction. `consoleonly_decide()` has no hardware in it and is unit tested on the host.

## Glitch detector (`glitchdet.c`)

The RP2350 has four detector circuits that reset the chip when its clock or core supply is
disturbed (datasheet section 10.9). A secured board would arm them for good with an OTP bit.
`glitchdet.c` arms them by software instead, from a setting, to find out in normal use
whether they ever reset a healthy board (`docs/security/decisions.md`, decision 5).

- **On by default:** armed at sensitivity 3, the highest, unless the setting says otherwise
  (`glitch off`, `glitch on <0-3>`, `glitch watch <0-3>`; `glitch default` goes back). A
  detector reset that nobody forced is a finding to report: see "Glitch detector resets" in
  [`docs/development.md`](../../docs/development.md).
- **Two modes:** *watching* (`glitch watch <0-3>`) runs the detectors at a sensitivity
  without arming them, and logs a trigger without resetting anything; *armed* (`glitch on`)
  resets the board on a trigger, as the OTP flag would.
- **Not protection:** it is armed after the configuration loads, software can disarm it, and
  the guard below disarms it after repeated resets, which is what deliberate glitching causes.
- **What survives a detector reset** is kept in POWMAN scratch registers 0 to 4 (that reset
  clears the watchdog's scratch registers, and so the fault record): the number of detector
  resets since power-on, how many came in a row soon after boot, the uptime, and the latest
  chip temperature and supply voltage (below). The uptime is rewritten about once a second,
  the readings at every pass of the main loop.
- **Reported at boot** (log, `uptime`, `glitch`): that the last reset was the detector's, which
  detectors fired, after how long, and the chip temperature and supply voltage (the last
  readings before it). Watching, each trigger's log line is followed by one with fresh readings.
- **The guard:** three detector resets in a row, each within three minutes of starting, and
  the next boot doesn't arm it, so the board stays reachable to turn the setting off.

## Chip temperature and supply voltage (`sensors.c`, `sensors_board.c`)

The board's own readings, for the week-long glitch detector test (issue #92), `temp` and
`power`: the chip's temperature from the RP2350's sensor (datasheet section 12.4.6), the supply
voltage VSYS through the Pico 2 W's divider on ADC 3, and whether USB power is present, from
the Wi-Fi chip's VBUS sense pin. Nothing else uses the ADC; each reading selects its own input.

- **A reading of each every 10 s** from the main loop: 64 conversions averaged (about 130 us),
  in tenths of a degree and millivolts. The lowest and highest since boot are kept with their
  uptimes, for `temp` and `power`.
- **VSYS shares its pin with the Wi-Fi chip's SPI clock.** The divider reaches the pin through
  a FET that conducts only while the Wi-Fi chip is deselected, so VSYS is read under the Wi-Fi
  driver's lock (`cyw43_thread_enter`), as the SDK's board header says. The pin is switched to
  the ADC for the reading; the driver gives it back to PIO at the start of its next transfer.
  The first conversions after the switch read low (on dev-1 the first by up to 15%, the second
  by 0.5%), so four are thrown away. Tried at 45 readings a second under heavy ping traffic:
  no more packets lost than without. VSYS can't be read before the Wi-Fi chip is running, so
  the main loop takes the first reading.
- **The first temperature reading waits 10 ms** after the sensor is switched on. Read at once it is about
  4 degrees high, settling within 2 ms (measured on dev-1). That first reading, taken before
  Wi-Fi starts, is kept for the glitch detector until the main loop's first, but isn't counted
  in the lowest and highest: on dev-1 it reads 0.6 to 0.8 degree above every reading after
  the Wi-Fi chip starts (most likely its load on the 3.3 V supply, the ADC's reference).
- **The log** gets one line with both readings: at boot; then whenever the temperature has
  moved a degree or the supply 0.2 V from the last line (at most one a minute); at once when
  USB power comes or goes; and at least one an hour: `chip temperature 26.2 C, supply 4.57 V,
  USB power (since boot: 26.2 C to 26.8 C, 4.56 V to 4.57 V)`. At notice level, so a serial
  logger on the console records the curve for a long test; the 4 KB history would not hold a
  week of it. The supply's step is above the dips a Wi-Fi transmission causes on USB power
  (about 0.13 V on dev-1), so traffic alone doesn't add lines.
- **For the glitch detector**, `glitchdet.c` copies the latest readings into POWMAN scratch
  registers 3 and 4, and takes fresh ones when a detector triggers while watching.
- **Limits**, stated by `temp` and `power`. The temperature is the chip's, not the room's
  (warmer than the air, more so under Wi-Fi load), and uncalibrated: the conversion assumes
  the ADC reference is exactly 3.3 V, and 1% off moves the reading by about 4 degrees; the
  sensor also varies from chip to chip. Good for change over time on one board, not for an
  absolute figure or for comparing boards. The supply is VSYS, the board's input: on USB power
  it's the USB voltage less a diode's drop (about 0.35 V). Measured through a 1% divider
  against the same reference, it's within a few percent. A reading taken during a Wi-Fi
  transmission catches its dip, so the lowest since boot can be a brief one.
- **Calibration** (`temp cal`): the configuration's `temp_offset`, tenths of a degree, is added
  to every temperature reading, so the log line, `temp` and the glitch detector's reports all
  carry it. `temp cal <degrees>` sets it to the reference minus a fresh uncalibrated reading.
  When it changes (or `config reset` clears it), `sensors_temp_offset_changed()` moves the
  lowest, highest and latest readings by the difference, so they stay comparable. It takes
  two bytes of the configuration record that were padding, so the record's length doesn't
  change. Offset only: the slope and the ADC reference stay as they are.
- `sensors.c` (the conversions, text, lowest and highest, when to log, the scratch register
  form) has no hardware in it and is unit tested on the host.

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

## Health checks (`health.c`, `health_board.c`, `hwtest.c`, `selftest.c`)

`health` (docs/design/health.md, #233): self-checks that report PASS, WARN or FAIL (or SKIP)
with a one-line reason. `health.c` is pure C and host tested (`tests/test_health.c`): the
registry, the results, the scheduling, the gates, the `health` table's text, the `sos.health()`
record, and the functions that judge the seed checks' numbers. It reads the clock through the
hook given to `health_init()`. `health_board.c` has the firmware's table and the steps that
gather the numbers. The boot tier's hardware and crypto checks keep their pure halves apart:
`hwtest.c` (the TRNG's startup tests and the judging of `trng`, `clocks` and `flash`;
`tests/test_hwtest.c`) and `selftest.c` (the crypto known answers over wolfCrypt, run against the
host's wolfCrypt by `tests/test_selftest.c`).

- **Tiers:** `health_boot()` runs the boot checks in `main()` right after `wolfCrypt_Init()`,
  before anything can make a key; each finishes in one call (a check that asks for a second is
  recorded as FAIL), and one log line sums the tier up with its time. `health_poll()`, after
  `sensors_poll()` in the main loop, does at most one step of one background check per pass,
  the most overdue first; a check with more work returns `HEALTH_MORE` and is stepped again on
  later passes. `health run` and `health full` are a shell job over `health_job_step()`, with a
  budget per call (20 ms); while it runs, `health_poll()` starts nothing, and starting it
  cancels a background check that was part-way through, so a check's state has one owner.
- **Results and logging:** the latest result counts. A change of status is logged at `kern`:
  FAIL at err, WARN at warning, back to PASS at notice (`(was FAIL 3 min ago)`). NONE to PASS
  or SKIP isn't logged, so a healthy board logs only the boot summary.
- **Gates:** `health_gate(HEALTH_GATE_KEYS | HEALTH_GATE_CRYPTO)` names the first FAILed check
  that holds the gate, or NULL. The callers that make keys or seal ask it first, refuse, and log
  the refusal with the check's name: the root secret (`rootflash.c`'s random source, so
  `rootsecret.c` stays pure), the burn image's secret (`rootburn_board.c`'s random source), the
  owner secret and the sealing of the configuration (`settings.c`; `settings_save_refused()`
  lets each caller of `settings_save()` say `the configuration wasn't saved: health check
  <name> failed`), the console password's salt, the host key (`hostkey.c`, asked again once the
  gate opens), and an encrypted card format (`sdcard.c`, before anything is written). On a
  fresh board whose root secret was refused, the console's rescue state names the check
  (`rootflash_refused_by()`) and allows `health`. Reading what is stored is never gated.
- **Seed checks** (phase 1): `resets` (boot), `glitch`, `stack`, `heap` (with hysteresis:
  WARN under 64 KB, PASS again only above 96 KB), `chip-temp`, `supply` (the lowest VSYS since
  its last run, `sensors_vsys_min_take()`), each every 60 s. They read state only and hold no
  gate. The thresholds are in `health.h`.
- **Boot checks** (phase 2, docs/design/health.md §9.2), after `resets`: `clocks` (each running
  clock through FC0 against `clock_get_hz()`, WARN past 1 %, FAIL past 5 %; FC0 counts against
  the crystal, so only the LPOSC's wide band shows a crystal about 2x off), `flash` (the JEDEC
  ID through `flash_safe_execute()` and `flash_do_cmd()`; FAIL if smaller than
  `PICO_FLASH_SIZE_BYTES`), `trng` (holds KEYS: SP 800-90B repetition count and adaptive
  proportion over 3072 raw oscillator bits read from the TRNG block under pico_rand's spin lock,
  one sample per 256 cycles, the corrector and the block's autocorrelation test bypassed, its
  CRNGT on; H = 0.5, alpha = 2^-30; the block is reset before and after), `crypto` (holds KEYS
  and CRYPTO: the boot subset of `selftest.c`'s 13 known answers, SHA-256, SHA-512, HMAC,
  AES-GCM with a refusal test, XTS and the Ed25519 public key). The rest (Ed25519 signing,
  verifying with a refusal test, X25519: tens of ms each) is `crypto-ssh`, a background check
  30 s after boot and then daily, holding the same gates. At boot `crypto` runs in one call;
  elsewhere both run one known answer per step. About 2 KB of heap while they run. Their text
  gives the time per group, and a passing run is logged at info. Thresholds and cutoffs are in
  `hwtest.h`.
- **Test builds** (`-DSOS_TEST_COMMANDS=ON`): `health inject <name> fail|clear` sets a result by
  hand; `health inject gate keys|crypto` adds a test-only full-tier check holding that gate at
  FAIL, to see each refusal on the board (`health full` runs it, and it passes, which opens the
  gate again). `health crypto-times` times every known answer; `health trng-diag` runs the TRNG
  test under 12 setups (sample period, autocorrelation test, corrector) to compare with the
  check's.
- **RAM:** about 100 bytes per registered check (the result and its text, scheduling); 24 at
  most.

## Stack monitor (`stackmon.c`)

`stackmon_paint()`, the first call in `main()`, fills the unused part of the core 0 stack with
a pattern. `stackmon_peak()` finds the deepest point that has been overwritten since, and `mem`
shows it. Interrupt handlers use the same 8 KB stack.
