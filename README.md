
# Small/Secure OS

## Objective

SOS turns a Raspberry Pi Pico 2 W into a small, headless, low-power general purpose computer
that you use remotely over Wi-Fi. There is no screen or keyboard: the device sits on the
network, mostly asleep, and you connect to it to get basic tasks done.

### Core capabilities

- **Remote shell**: log in over the network to a command shell to run commands, inspect the
  system and manage files.
- **Communication**: tools for sending and receiving messages with people and other systems
  over standard messaging protocols, encrypted and authenticated like every other connection.
- **File transfer**: upload files to and download files from the device's storage (microSD
  card on the Adalogger).
- **Timekeeping**: keep accurate wall-clock time across power loss using the Adalogger's
  battery-backed RTC, synchronised over the network with authenticated NTP (NTS).

### Design principles

- **Secure by default**: every network connection is encrypted and authenticated; no
  plaintext logins or protocols. Credentials and keys never leave the device unencrypted.
- **Low power**: idle in low-power sleep, wake on network activity or timers, use Wi-Fi power
  saving, and do as little work as possible while nobody is connected.
- **Headless**: fully usable and recoverable without a screen; the USB serial console is for
  development and recovery only.
- **Small**: fit comfortably in the RP2350's 520 KB of RAM and 4 MB of flash, with bulk data
  on the microSD card. Prefer a few well-chosen features over many partial ones.

### Non-goals

- Graphical interfaces, displays or local input devices.
- Running unmodified Linux/POSIX software.
- Serving many users or many simultaneous connections.


## Hardware

- RPI Pico2W
- Pi Cowbell Adalogger - https://www.adafruit.com/product/5703 (RTC and microSD slot)

## Software Stack

- [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) 2.3.1
- `arm-none-eabi-gcc` toolchain, CMake (3.13+), Ninja
- `picotool` for flashing, `openocd` (Raspberry Pi fork) for SWD debugging
- [wolfSSL](https://github.com/wolfSSL/wolfssl) 5.9.4 and [wolfSSH](https://github.com/wolfSSL/wolfssh) 1.6.0
  for cryptography, TLS and the SSH protocol
- [no-OS-FatFS-SD-SDIO-SPI-RPi-Pico](https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)
  for the SD card driver and FatFs
- [Lua](https://www.lua.org) 5.4 for scripts
- lwIP and the CYW43 Wi-Fi driver, from the Pico SDK
- SSH server glue in `src/ssh/`, derived from [pico-sshd](https://github.com/tjko/pico-sshd)
  (see [`THIRD_PARTY.md`](THIRD_PARTY.md))

wolfSSL, wolfSSH, the SD card library and Lua are git submodules in `lib/`.

The source code is grouped by subsystem under `src/`; [`src/README.md`](src/README.md) gives
the overview (main loop, interrupt context, core 1) and each directory's README explains how
its subsystem works. Reference documentation (datasheets, SDK manuals) lives in
[`docs/`](docs/README.md).

## Setup

1. **Toolchain:** install `arm-none-eabi-gcc` with newlib, CMake, Ninja and
   [`picotool`](https://github.com/raspberrypi/picotool) (2.x, for RP2350). See
   [Getting started with Raspberry Pi Pico](docs/sdk/getting-started-with-pico.pdf) for
   per-OS instructions.

2. **Source,** including the submodules (wolfSSL, wolfSSH, the SD card library and Lua):

   ```sh
   git clone --recurse-submodules <repository-url> sos
   # or, in an existing clone:
   git submodule update --init
   ```

3. **Pico SDK:** point `PICO_SDK_PATH` at a Pico SDK 2.3.1 checkout (with its submodules), or
   let the build download it by configuring with
   `-DPICO_SDK_FETCH_FROM_GIT=ON -DPICO_SDK_FETCH_FROM_GIT_TAG=2.3.1`.

   ```sh
   git clone -b 2.3.1 --recurse-submodules https://github.com/raspberrypi/pico-sdk.git
   export PICO_SDK_PATH=$PWD/pico-sdk
   ```

4. **Optional, for development: a provisioning file.** The firmware image holds no
   configuration (no Wi-Fi password, no keys): a board is set up on its serial console (see
   [Configuration](#configuration)). To set one up without typing, describe it in
   `provision.conf` (git-ignored) and write that to the board after flashing (see
   [Provisioning](#provisioning)):

   ```sh
   cp provision.conf.example provision.conf
   $EDITOR provision.conf   # ssh_key is the full line from ~/.ssh/id_ed25519.pub
   ```

To keep wolfSSL/wolfSSH outside the repository, develop in a VM, or debug over SWD, see
[`docs/development.md`](docs/development.md).

## Building

```sh
cmake --preset debug          # or: release
cmake --build --preset debug  # produces build/sos.uf2 and build/sos.elf
```

## Flashing

```sh
cmake --build --preset debug --target flash
```

This runs `picotool load -f -v -x`, which reboots a running SOS image into BOOTSEL over USB,
loads, verifies and starts it. For a board running some other firmware, hold BOOTSEL while
plugging it in first. Flashing leaves the board's saved configuration, SSH host key and SD
device key alone; a new board has neither a network nor a trusted key until it's set up
(`setup` on the console, or [provisioning](#provisioning)).

Developing inside a VM? See [`docs/development.md`](docs/development.md) for forwarding the
Pico's USB connection so flashing works unattended.

## Console

The console runs over USB CDC (`/dev/ttyACM0` on Linux) and is mirrored on UART0 (GP0 TX / GP1 RX):

```sh
picocom -b 115200 /dev/ttyACM0
```

`help` lists the commands. [docs/commands.md](docs/commands.md) is the full reference, and marks
which commands work only on the console. The greeting above the first prompt (and after each SSH
login) names the commit the firmware was built from, e.g. `SOS 9332d0364fb3`; `version` shows it
too, with `-dirty` if the build had uncommitted changes.

## Configuration

The Wi-Fi network, hostname, SSH user and trusted public keys are set on the serial console
(USB or UART) and saved in flash. On a new board, connect to the console and run `setup`:

```
sos> setup
Wi-Fi network (SSID): my-network
Wi-Fi password (empty for an open network):        (not echoed)
SSH user name [admin]: julio
Public key: ssh-ed25519 AAAAC3Nza... julio@laptop    (paste ~/.ssh/id_ed25519.pub)
```

| Command | |
|---|---|
| `config` | show the configuration |
| `wifi` / `wifi set` / `wifi connect` | show / change the network (password not echoed) / reconnect |
| `wifi power on` / `wifi power off` | Wi-Fi power saving (default off; see below) |
| `wifi country US` / `wifi country worldwide` | radio rules to follow (two-letter country; default worldwide; after reboot) |
| `wifi stats` / `wifi stats chip` | signal, noise and packet counters, for chasing packet loss / plus the chip's raw counters |
| `wifi keepalive on` / `wifi keepalive off` | address announcement (see below); off is for testing and lasts until reboot |
| `key list` / `key add [line]` / `key del <n>` | trusted keys (ssh-ed25519, ecdsa-sha2-nistp256; up to 8) |
| `hostname <name>`, `user <name>` | hostname (applies at the next Wi-Fi connect), SSH user name |
| `time auto <hours>` / `time auto off` | periodic NTS sync (default off; see [Timekeeping](#timekeeping)) |
| `time cal <ppm>` / `time cal off` | RTC calibration: how fast the RTC runs, in ppm (default off; see [Timekeeping](#timekeeping)) |
| `time server <name>` / `time server default` | NTS server: host name or IPv4 address / back to `SOS_NTS_SERVER` |
| `config reset` | erase everything: no Wi-Fi network, no trusted keys, hostname and user back to `sos` and `admin` |

- **Changes only on the console.** SSH sessions can view the configuration but not change it,
  so a remote session can't lock itself out; physical access to the console is the trust
  boundary (it already allows reflashing).
- Changes apply immediately: a new key works for the next login, `wifi set` reconnects.
- Wi-Fi connects in the background, retries after failures (wrong password, network not
  found) and reconnects if the link drops; the console is usable throughout.
- On a network with several access points (a mesh), the device scans when connecting and
  ends up on the strongest one; the log names it and its signal.
- Wi-Fi power saving is off by default. With it on, the radio sleeps between beacons when
  idle: lower power, but the first packet after a quiet spell can take a little longer. It
  applies at once.
- While connected, SOS announces its address once a minute (a gratuitous ARP). Without it, a
  device that sends nothing stops receiving broadcasts (seen with the Pico 2 W within minutes,
  power saving on or off), so after the router forgets it (~20 minutes here) clients get
  "no route to host". `net` shows ARP packets received and sent; `wifi keepalive off` turns
  the announcement off until the next reboot, for testing.
- Storage: one record in two alternating flash sectors below the key sector, with a sequence
  number and CRC, so a power cut while saving keeps the previous version. It survives
  reflashing. The Wi-Fi password and keys are stored unencrypted.
  The record format only grows at the end: a record saved by older firmware still loads, with
  newer settings at their defaults. Firmware older than a saved record ignores it and starts
  empty.
- The firmware has no configuration of its own. With nothing saved (a new board, or after
  `config reset`) there is no network and no trusted key, so no SSH, until `setup` on the
  console or provisioning.

### Provisioning

For development and automated tests, a board's whole configuration can be written from this
machine instead of typed:

```sh
ninja -C build provision          # from provision.conf, or the file named by $SOS_PROVISION_FILE
tools/provision.sh other.conf     # the same, from another file; -n only builds and checks it
```

`provision.conf` (git-ignored; see `provision.conf.example`) lists the Wi-Fi network, hostname,
SSH user, trusted keys and the optional settings. The script builds the saved record on this
machine, with the same code the device uses, and loads it into the two configuration sectors
over USB with `picotool`, like flashing: the board reboots into BOOTSEL and back. It replaces
everything saved on the board; the firmware and the SSH host key stay as they are.

- `ninja flash` doesn't touch the configuration, so provisioning is needed once per board, and
  again after `config reset`, a full flash erase, or to put a board into a known state for a
  test.
- Provision from the source tree the board's firmware was built from: firmware older than the
  record's format ignores it and starts empty.
- Like the console, this needs physical access, which already allows reflashing. Nothing on
  the network can provision a board.

## Logging

Boot, Wi-Fi, configuration and SSH server messages go to the system log:

- The serial console prints them as they happen. A message arriving while you type clears your
  input line, prints, and redraws the prompt and what you typed (a hidden password isn't
  redrawn).
- `dmesg` shows the last 4 KB of messages in any shell, SSH sessions included.
- `log console <level>` sets what the console prints (default `notice`); `log history <level>`
  what's kept for `dmesg` (default `info`). Levels: err, warning, notice, info, debug. Console
  only to change; `log` shows the levels in any shell.
- Only printable ASCII is logged: other bytes in quoted text (a user name, a file name) become
  spaces or `?`.

## Remote access (SSH)

Once the device has a Wi-Fi network and at least one trusted key, it starts an SSH server on
port 22. The log shows the IP address and the host key fingerprint:

```
wifi: connected, ip 192.168.1.89
sshd: listening on port 22 (up to 3 sessions), host key ssh-ed25519 SHA256:...
```

Check that fingerprint (also shown by the `ssh` shell command over USB) against the one your
client prints on first connection, then log in:

```sh
ssh <user>@<device-ip>
```

- Only public-key login is accepted: the configured user with one of the trusted keys.
- The Ed25519 host key is generated on the device at first boot and stored in the last 4 KB
  flash sector (the key sector, which also holds the SD cards' device key); it survives
  reflashing. The private key never leaves the device.
- Up to 3 concurrent sessions (`SOS_SSH_MAX_SESSIONS`). While all are in use, further
  connections are refused with an SSH disconnect message the client displays
  ("Too many connections: all 3 SSH sessions are in use, try again later"). Each session gets
  its own shell, separate from the others and from the USB console.
- Connections that don't log in can't hold the server. A connection has 30 s to log in, but
  only 5 s while every session is in use. An address whose connections end without a login 30
  times within a minute is refused (its connections are reset) until it has stayed away for a
  minute; successful logins don't count. `ssh` shows how often either happened.
- Keys and nonces come from wolfCrypt's Hash-DRBG, seeded from the RP2350's hardware random
  number generator through the SDK.
- `help` lists commands; `ssh` shows the active sessions (yours marked `*`) and `who` who is
  logged in; `logout` or Ctrl-D ends the session.

## Messages between sessions

The people logged in (the console and each SSH shell) can message each other:

- `wall <message>` shows a message in every other shell at once.
- `chat` joins a chat room: each line typed at the `chat> ` prompt goes to everyone else in it.
  `/who` lists the room; `/quit`, Ctrl-C, or Ctrl-D on an empty line leaves.
- `who` lists each shell: slot, name, where it's connected from, whether it's in the chat.

Each person is named after the comment of the trusted key they logged in with (`alice@laptop`;
`key <n>` without one), the console is `console`, and a second login with the same key gets its
slot added (`alice@laptop/2`). A message arrives between keystrokes: the line being typed is
redrawn after it, and a full-screen `edit` holds messages until it quits. Messages are kept only
in memory (the last 16), never in the log or on the card. A client that stops reading holds
a sender up for at most 2 seconds; after that it misses messages, and is told how many. See
[docs/commands.md](docs/commands.md#messages) and [`src/chat/`](src/chat/README.md).

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

## Timekeeping

The Adalogger's PCF8523 RTC (I2C0: SDA GP4, SCL GP5), kept running by its CR1220 coin cell,
is the source of truth for wall-clock time. At boot SOS sets the system clock (`time()`,
`gettimeofday()`) from it and turns on the chip's battery switch-over, which is off by default.
All times are UTC.

Network time comes from **NTS** (Network Time Security, RFC 8915): NTP whose replies are
authenticated. SOS does a TLS key exchange with the NTS server (TCP port 4460), then sends
NTP requests that carry a single-use cookie and an AES-SIV tag, and accepts only replies that
check out under the server's key. Each sync sends 4 queries and uses the one with the shortest
round trip, which keeps Wi-Fi power-saving delays out of the result. The server is set on the
console with `time server <name>`; the default is the build-time `SOS_NTS_SERVER`
(`time.cloudflare.com`). The server's certificate chain must end at a trusted root: built in
(`src/tls/tls_roots.c`: SSL.com Root Certification Authority ECC for Cloudflare, ISRG Root X1 for
Let's Encrypt, which most other NTS servers use) or approved from the SD card (see
[Trusted root certificates](#trusted-root-certificates)).

SOS syncs in three cases:

- **The RTC has no valid time:** no chip, a first power-up, or a coin cell that ran out while
  the board was unpowered. A failed sync is retried after 30 s, then with doubling delays up to
  30 minutes, until one works.
- **`time sync`**, typed in any shell, SSH included.
- **Periodically, if turned on** with `time auto <hours>` on the console (1 to 720; default off).
  It syncs one interval after the last successful sync, whatever started it, or after boot if
  there has been none: a restart doesn't sync by itself when the RTC had the time, so a board
  that restarts often doesn't keep asking the server. This corrects RTC drift (see
  [RTC calibration](#rtc-calibration)) on a board that stays up. Without it, or on a board that restarts more
  often than the interval, run `time sync` now and then.

A successful sync sets the system clock and the RTC. NTS time is authenticated, so a sync may
move the clock any distance; a correction of more than 5 minutes is logged as a warning. Any
time earlier than the firmware's configure time is rejected.

While the clock isn't set, certificate dates can't be checked, so that first key exchange
accepts certificates whatever their dates (signatures and the host name are still checked) and
logs `certificate dates not checked (clock not set)`. Once the clock is set, dates are checked
as usual.

There is no automatic fallback to plain, unauthenticated NTP: if the NTS server can't be
reached (for example because the network blocks port 4460) and the RTC has no time, the clock
stays unset until NTS works. `time sync sntp` (console only) does one plain SNTP sync on request,
from the build-time `SOS_NTP_SERVER` (`pool.ntp.org`); `time` then shows the time as
unauthenticated.

- `time` shows the date and time and where it came from (RTC, NTS, or SNTP), the RTC's reading
  and backup-battery state, the RTC calibration, the last sync with its correction and round
  trip, a failed attempt and when it's retried, and the server and periodic settings.
- `time auto`, `time server` and `time cal` (without a value) show those settings; `config`
  lists all three.
- `nts` shows the NTS state (cookies, last key exchange and query). `nts ke [host]` and
  `nts query` run the two steps by hand, for diagnosis; they don't set the clock. Naming another
  host is console only.

### RTC calibration

The RTC's crystal isn't trimmed, and an uncalibrated one can be well off: the board this was
developed on ran 92 ppm fast, about 8 s a day. The PCF8523 can correct its own rate, and
`time cal` (console only) sets that:

1. **Measure.** Run `time sync`, leave the board for a day or more, restart it (so the clock
   is taken from the RTC again), and run `time sync` once more. `time` then shows the step, for
   example `clock stepped -5419 ms`. A negative step means the RTC ran fast. The RTC counts
   whole seconds, so the step is only good to about a second: the longer the wait, the better.
2. **Work out the rate:** ppm = milliseconds stepped ÷ seconds waited × 1000, positive if the
   RTC ran fast. 5419 ms in 59,051 s is 92 ppm.
3. **Set it:** `time cal 92`. The RTC is slowed by the nearest step it can do (4.34 ppm each,
   here 91.1 ppm), and `time` shows `cal: slowed by 91.1 ppm`. A slow RTC takes a negative
   value.

The value isn't added to the one before: after measuring again with a calibration set, give
the rate `time` shows on its `cal:` line (91.1 here, not the 92 that was typed) plus the new
error. `time cal off` removes it. The calibration is a saved
setting (`rtc_cal_ppm` when provisioning), written into the RTC at boot if the chip doesn't
already have it, so it survives a dead coin cell. The RTC applies it as a few short
corrections every two hours.

## Files (SD card)

The Adalogger's microSD slot (SPI0: SCK GP18, MOSI GP19, MISO GP16, CS GP17, at 25 MHz) holds
the device's files. Cards over 2 GB (SDHC/SDXC) with exFAT or FAT32 work; the card is mounted
when first used. The file commands work in every shell, SSH included, and each shell has its own
working directory:

| Command | |
|---|---|
| `ls [path]`, `cd [dir]`, `pwd` | list, change and show the working directory |
| `cat <file>` | print a file |
| `cp <from> <to>`, `mv <from> <to>` | copy, move or rename (to a directory: keeps the name) |
| `rm <path>`, `mkdir <dir>` | delete a file or an empty directory, create a directory |
| `echo <text> [> file \| >> file]` | print, or write/append a line |
| `sd` | card and volume information, whether it's encrypted, SPI clock |
| `sd format [exfat\|fat32] [plain]` | console only: erase the card and format it (one partition), encrypted unless `plain`; an encrypted format shows the card's key once, for offline recovery |
| `sd devkey [forget]` | the device key's state; console only: clear it |
| `sd test [KB [keep]]`, `sd speed [MHz]` | console only: write/read benchmark (everything else waits meanwhile); SPI clock |
| `sd unmount` | console only: unmount the card, to remove it |

- **Long commands run in steps.** `ls`, `cat` and `cp` do a little work per pass of the main
  loop, so SSH sessions, the console and the watchdog keep going; Ctrl-C stops them (a stopped
  `cp` removes its partial copy). Commands typed meanwhile run afterwards.
- **A slow SSH client** slows `cat` down instead of losing output. Printing a file over SSH runs
  at about 130 KB/s (encryption-bound); copying on an unencrypted card at about 580 KB/s.
- **Open files are protected:** a file that a running command has open can't be deleted or
  renamed, and the card can't be formatted or unmounted meanwhile.
- File names with spaces aren't supported by the commands yet. Files are dated with the system
  clock (UTC), which comes from the RTC or NTS.
- No card-detect switch is wired by default (solder jumper SJ1 connects it to GP15), so SOS
  doesn't notice a card swap: run `sd unmount` before taking the card out; the next file command
  initializes and mounts the new card.

### Encrypted cards

`sd format` encrypts the card by default, so a card taken out of the device can't be read
without it. Everything written to the card from then on is encrypted, file names included;
FatFs, the file commands, SFTP and Lua work as before. Formatting doesn't erase the card's old
contents, though: files from earlier, unencrypted use stay readable on it until new data
overwrites them, so a card that held private files unencrypted is best replaced by a new one.
Cards formatted elsewhere, or with `sd format plain`, still work unencrypted; `sd` shows which
kind is in.

- **The key:** each card has a random data key, stored on the card encrypted with this device's
  key. The device key is made when the first card is encrypted and kept in flash next to the
  SSH host key, so it survives reflashing, `config reset` and provisioning, and the card
  unlocks at boot with nobody logged in. A card made by another device is refused (`sd format`
  erases it for use here). The card holds two copies of its header; a damaged first copy is
  rewritten from the second.
- **What it protects:** a lost or removed card. Not a stolen device: the device key is in its
  flash, which can be read out.
- **Recovery:** an encrypted `sd format` prints the card's id and data key when it finishes, and
  only then: no command shows it later. (The device key that unlocks the card is still in the
  device's flash, which anyone with the device can read out.) Store it offline: it's the only
  way to read the card if the device is lost. With it, a Linux PC reads the card
  (`tools/host/sdcrypt.c` decrypts an image of it with the key in a file, and `sdcrypt info`
  prints the `cryptsetup` line to open the card directly). A card whose key wasn't saved can't
  have it shown again; formatting it anew shows a new key (and erases the card).
- **The device key is never replaced on its own.** If its flash record is damaged (or was
  written by newer firmware), encrypted cards stay locked and `sd devkey` says so; `sd devkey
  forget` clears it, after which only the keys shown at format read the old cards. Firmware
  from before encryption erases the whole key sector if it ever has to make a new SSH host key,
  which would take the device key with it.
- **Speed:** the encryption is software AES (the RP2350 has no AES hardware): about 690 KB/s
  write and 650 KB/s read on the card, against 2.0 / 1.7 MB/s unencrypted. Copying on the card
  slows down by about as much; SFTP is limited by SSH's own encryption and barely changes.
- A PC sees an encrypted card as unformatted. Don't let it format the card.

### File transfer (SFTP)

The SSH server also serves SFTP on the card, with the same login:

```sh
sftp <user>@<device-ip>            # or scp -s, FileZilla, WinSCP ...
sftp> put photo.jpg
sftp> get notes.txt
```

- Upload, download (resumable: `reput`, `reget`), `ls`, `mkdir`, `rmdir`, `rm`, `rename`. The root
  is the card's root. Changing attributes (`chmod`, times) isn't supported: FAT has no Unix
  permissions.
- About 170 KB/s up and 120 KB/s down (2 MB files; encryption-bound). Each request is handled in
  one pass of the main loop: a 32 KB read reply holds the console and other sessions for up to
  ~0.1 s.
- Each session uses up to ~100 KB of heap while transferring. A transfer's open files count as in
  use, like a running `cp` (see above); a client that disconnects mid-transfer releases them.
- Every change to the card, from SFTP or the file commands, passes one check,
  `files_write_allowed()` in `src/files/files.c`. Everything is allowed except changing `/certs`, which
  only the console may do (below).

### Trusted root certificates

TLS (used by NTS and the HTTPS client) trusts the built-in roots and, optionally, extra ones
from the card in `/certs/roots.pem`: a PEM bundle (text between the certificates, as in system
CA files, is fine) or DER certificates, up to 64 KB and 32 certificates. The file is used only
while it's exactly the file approved on the console:

1. Upload it anywhere with SFTP, e.g. `put roots.pem incoming/roots.pem`.
2. On the console: `mkdir /certs` (once), then `mv /incoming/roots.pem /certs/roots.pem`.
3. On the console: `tls roots approve`. It lists each certificate (common name, key type, whether
   it's a CA, expiry, SHA-256 fingerprint) and checks TLS can use it; type `yes` to trust them.
   The file's SHA-256 is saved with the configuration.

- **Only the console can change `/certs`.** SSH shells and SFTP can read it but not write,
  delete or rename anything in it, under any spelling FatFs accepts (`/CERTS.`, `\certs`,
  `/x/../certs`).
- **A changed file isn't used** until it's approved again: TLS then logs a warning once and uses
  the built-in roots alone. The same happens if the file or the card is missing, so NTS keeps
  working without the card.
- `tls roots` shows the built-in roots and the file's state; `tls roots forget` (console) stops
  trusting it. A private key, or any PEM block other than a certificate, makes the file invalid.

## Editor

`edit <file>` opens a file of up to 32 KB in a full-screen editor, on the console or over SSH
(a VT100/xterm-compatible terminal; it uses the alternate screen, so the shell's output is back
afterwards). Ctrl-S saves, Ctrl-Q quits (twice with unsaved changes), Ctrl-F finds, Ctrl-L
redraws and asks the terminal for its size again. UTF-8 is fine, wide characters take two
columns, and CRLF line ends are kept.

- **Saving** writes `<file>.~edit` and then replaces the file, so an interrupted save never
  leaves half a file; after a power cut, `edit <file>` picks the text up from the `.~edit` copy.
- While the editor runs on the console, log messages wait (the status bar counts them) and appear
  when it quits; `wall` and `chat` messages wait too.
- `/certs` can be opened from SSH but saved only from the console.

The keys and details are in [docs/commands.md](docs/commands.md#editor) and
[`src/edit/`](src/edit/README.md).

## HTTPS client

The device can make HTTPS requests: from Lua scripts with `sos.http` (below), and from any shell
with `http get|head <url>` and `http post <url> <text>`, mostly for testing. Every request uses
TLS 1.3 with the server certificate checked against the trusted roots
([above](#trusted-root-certificates)), and redirects aren't followed. The `http` command
filters what the server sends before it reaches the terminal (no escape sequences, invisible or
bidi characters); `sos.http` gives a script the bytes as they are.

- Two HTTPS requests can run at once; a third is refused ("too many HTTPS requests at once")
  until one finishes. One more TLS connection is always kept free for time sync (NTS).
- A request is refused while the heap is too low for a TLS handshake (about 64 KB free).

See [docs/commands.md](docs/commands.md#https) and [`src/net/`](src/net/README.md).

## Lua scripts (prototype)

Lua 5.4 runs on the second core (core 1), so a script can compute flat out without slowing the
console, SSH or the watchdog on core 0. Scripts live on the SD card:

```sh
lua -e 'print(1 + 1)'            # a one-liner
lua /scripts/report.lua          # a file (relative paths start at the working directory)
lua                              # an interactive prompt
```

At the prompt (`>`), each line runs as you enter it, in one Lua state for the whole session, so
variables and loaded modules stay. An expression's value is printed (`> 2^10` prints `1024.0`),
and a statement that isn't finished continues on the next line (`>>`). Ctrl-C interrupts the
running line, or drops an unfinished statement; Ctrl-D on an empty line, or `exit`, leaves. Lines
typed or piped in while one runs wait their turn, so `printf 'lua\nprint(1)\nexit\n' | ssh sos`
works too.

The script's output goes to the shell that started it; Ctrl-C stops it, as does ending the SSH
session. One script runs at a time.

**Background scripts and autorun:** `lua -b <file>` runs a script with no shell: its output goes to
the log, a line at a time (`dmesg`; INFO, so not on the console), and its end is logged
(finished, stopped, or failed with the error). `lua ps` shows the running script, and `lua stop`
stops it, wherever it runs. At most 10 lines a second of a background script's output reach the
log; the rest are counted ("... N lines dropped"), so a chatty script can't push everything else
out of the log history. If the card has `/autorun.lua`, it starts in the background a few seconds
after boot. After a crash or watchdog reset it's skipped (and that's logged), so a script that
brings the device down can't do it at every boot. It stays off until the next normal boot
(`reboot`, or power), even if the reset had nothing to do with it: on an unattended board, check
the log, or start it by hand with `lua -b /autorun.lua`. Remove or rename the file to turn
autorun off. Timers (`sos.every`, `sos.after`) are how a background script does periodic work:

```lua
-- /autorun.lua: log how many entries the card's top directory has, every 10 minutes
sos.every(600, function()
  local n = 0
  for _, e in ipairs(sos.list("/") or {}) do n = n + 1 end
  sos.log("autorun: " .. n .. " entries in /")
end)
```

Background scripts can write files like any other script (not `/certs`). Available: the base, string, table, math, utf8 and
coroutine libraries, `require` (see below), and `sos`:

| Function | |
|---|---|
| `sos.read(path)` | file contents as a string |
| `sos.write(path, s)`, `sos.append(path, s)` | write / append (through `files_write_allowed()`) |
| `sos.list([path])` | `{ {name=, size=, dir=}, ... }` |
| `sos.remove(path)`, `sos.mkdir(path)` | delete a file or empty directory, create a directory |
| `sos.time()`, `sos.uptime()` | seconds since 1970 (UTC), seconds since boot |
| `sos.sleep(ms)`, `sos.log(msg)` | wait (Ctrl-C still works), write to the system log |
| `sos.every(s, fn)`, `sos.after(s, fn)` | call `fn(id)` every `s` seconds (at least 0.01) / once after `s`; returns the timer's id. Timers run once the script's main code has returned: the script then keeps going until no timers are left, it's stopped, or a callback fails. Not at the prompt |
| `sos.cancel(id)` | stop a timer; true if it existed |
| `sos.http.get(url [, opts])` | HTTPS GET: `{status=, reason=, headers=, body=}` (see below) |
| `sos.http.post(url, body [, opts])` | HTTPS POST of the string `body` |
| `sos.http.request(opts)` | any method: `opts.url` and `opts.method` (default `GET`, or `POST` with a body) |

Failures return `nil` and a message. There's no `io`, `os` or `debug`: scripts reach the system
only through `sos`. `__gc` metamethods aren't supported (`setmetatable` refuses them; use a
`<close>` variable).

**Modules:** `require "name"` loads a Lua module from the card, searching `package.path`. The
default is `/lib/?.lua;/lib/?/init.lua;./?.lua;./?/init.lua`, where `./` is the working
directory and dots in the name become directories (`require "net.util"` tries
`/lib/net/util.lua`, ...). A script may change `package.path` before calling `require`.
Modules are loaded once per script (`package.loaded`), and `package.preload` works as usual.
Only Lua source is accepted: no precompiled chunks and no C modules.

```lua
-- /lib/greet.lua
local M = {}
function M.hello(who) return "hello, " .. who end
return M

-- a script
local greet = require "greet"
print(greet.hello("pico"))
```

**HTTPS:** `sos.http` makes one request at a time with the HTTPS client (`src/net/README.md`),
from scripts started on the console or over SSH. The server certificate is checked as for every
TLS connection, and redirects aren't followed (the script sees the 3xx and `headers.location`).
Any status is a response; `nil` and a message mean there was none (DNS, TLS, timeout, a bad
option, a body over `max_body`). Options, all optional except `url` for `request`:

| Option | |
|---|---|
| `headers` | extra request headers, `{["X-Name"] = "value"}`. `Content-Type` here is the same as `content_type`; `Host`, `Content-Length`, `Connection` and the like are set by the client and refused |
| `body`, `content_type` | request body (up to 16 KB) and its type (no `Content-Type` header if not given) |
| `timeout` | milliseconds until the headers arrive, then between pieces of the body (default 30000) |
| `max_body` | the most response body to accept, in bytes (default 32 KB; the script's 128 KB covers it) |

The response's `headers` table has lower-case names; a repeated header's values are joined with
`, `. The body is the server's bytes as they are: a script that prints it prints whatever the
server sent, escape sequences included.

```lua
local r, err = sos.http.post("https://ntfy.example/alerts", "disk almost full",
                             {headers = {Title = "sos"}})
if not r then print("failed: " .. err) elseif r.status ~= 200 then print(r.status, r.reason) end
```

- **Stopping:** Ctrl-C ends the script even if it catches errors with `pcall` or `xpcall`: from
  then on, any code it runs outside a `pcall` raises "interrupted" again. The exception is a
  single long call into C, such as a pattern match that backtracks for minutes, which can't be
  interrupted until it returns. Core 0, SSH and the watchdog keep running meanwhile, but no
  other script can start.

- **How it works:** core 1 never touches the card, network or shells. Each `sos` call is a
  request that core 0's main loop answers (one per pass; an HTTPS request is answered once its
  headers or the next piece of its body are in, with the main loop going on meanwhile); output goes through a buffer that the
  shell drains as its channel takes it. The heap is shared (malloc is mutex-protected), and each
  script may allocate up to 128 KB ("not enough memory" beyond). Core 1 gets a 32 KB stack from
  the heap when the first script runs, and pauses while core 0 writes flash.
- **Nesting:** plain Lua recursion is limited only by memory, but nesting through `pcall`,
  callbacks from C (`string.gsub`, `table.sort`), metamethods and coroutines uses core 1's C
  stack and stops at 20 levels with "C stack overflow". `mem` shows how much of core 1's stack
  the last script used.
- **Speed** (debug build): `fib(25)` takes 1.4 s; a million-iteration floating-point loop 2.6 s.
- **Prototype limits:** numbers are formatted with newlib's `snprintf`
  on core 1 (its float conversion has state that isn't safe to share between cores, and core 0
  doesn't use it today).

## Testing

**Unit tests** run on the development machine with the host compiler (no board, no Pico SDK),
under AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
```

They cover:

- **Shell:** line editing, questions and output (`src/shell/shell_core.c`, with the commands
  faked), terminal key decoding (`src/shell/keys.c`), character widths (`src/shell/charwidth.c`),
  and the filter that keeps outside text from sending the terminal escape sequences
  (`src/shell/termtext.c`).
- **Editor:** its text, keys and screen (`src/edit/editor.c`).
- **Messages:** `wall`, `chat` and `who` between shells (`src/chat/chat.c`).
- **SSH:** the session I/O ring buffer (`src/ssh/pico-sshd/ringbuffer.c`, including a randomized
  comparison against a reference model).
- **Storage:** the configuration store (`src/core/config.c`, with a fake flash that can fail or
  lose power mid-write), path handling for the file commands (`src/files/path.c`), and SD card
  encryption (`src/files/sdcrypt.c`, with wolfCrypt built for the host and cross-checked against
  `tools/host/sdcrypt.c`, which uses OpenSSL's libcrypto; that suite needs the wolfSSL source and
  OpenSSL's development files, and is skipped with a warning without them).
- **Time:** date and RTC register conversions (`src/time/timeconv.c`, compared against the host C
  library), the NTS key-exchange messages and NTS-protected NTP packets (`src/time/nts_ke.c`,
  `src/time/nts_ntp.c`).
- **TLS and HTTPS:** the root certificate bundle parser (`src/tls/certbundle.c`), which new TLS
  connections may open (`src/tls/tls_admit.c`), and the HTTPS client's URLs, requests and
  response parser (`src/net/http_msg.c`, including corrupted responses fed in random pieces).
- **Other:** the log (`src/core/log.c`) and the provisioning tool (`tools/host/mkconfig.c`).

`tests/test.h` is a minimal harness; add a suite as `tests/test_<name>.c` and register it in
`tests/CMakeLists.txt`.

**Hardware tests** run against a board that is flashed with SOS and on the network:

```sh
tools/test/ssh-hw-test.sh -u <user> <device-ip>     # -q for a quick run, -h for options
tools/test/sftp-hw-test.sh -u <user> <device-ip>    # needs a formatted SD card
```

`tools/test/ssh-hw-test.sh` checks the SSH server. It covers login and logout, authentication
(only public key; unknown key, wrong user and password refused), concurrent sessions kept
separate, the "too many connections" message, slot reuse, a large input burst, clients that
disappear, back-to-back logins, and heap use before and after. It exits with the number of failed checks. It uses every session slot for a few seconds,
so run it while nobody else is using the board; it warns if the device rebooted mid-run.

`tools/test/sftp-hw-test.sh` checks SFTP on the SD card. It puts and gets a 2 MB random file
and a small one, overwrites a file with a smaller one, resumes both ways (`reget`, `reput`), and
checks that a failed rename or rmdir is reported. During a download it checks that the card
is reported as in use and that removing the file is refused, then kills the client and checks
that the file is released. It ends by comparing heap use before and after. It works in
`/sftp-hw-test` (removed at the end) and exits with the number of failed checks. Run it while
nobody else is using the board.

## Memory layout

| Region | Use |
|---|---|
| Main RAM (512 KB) | data, heap, and the core 0 stack (top 8 KB, `SOS_CORE0_STACK_SIZE`) |
| SCRATCH_X (4 KB) | core 1 stack (`PICO_CORE1_STACK_SIZE`), as in the SDK default; while Lua runs, core 1 uses a 32 KB stack from the heap instead |
| SCRATCH_Y (4 KB) | free |
| Flash, last 4 KB sector | keys: SSH host key (page 0), SD cards' device key (page 1) |
| Flash, 2 sectors below it | configuration (A/B records) |

The core 0 stack is moved out of SCRATCH_Y by the linker script overrides in `config/ld/`.
Interrupt handlers, including lwIP/CYW43 network processing, share it. The `mem` shell command
reports the stack high-water mark; an SSH login peaks at about 3.2 KB. It also shows how deep the
last Lua script went into core 1's stack.

## License

SOS is licensed under the GNU General Public License v3.0 or later; see [`LICENSE`](LICENSE).
Code copied from other projects, the libraries linked into the firmware and their licenses are
listed in [`THIRD_PARTY.md`](THIRD_PARTY.md), including an open question about the Wi-Fi
driver's license that matters before distributing firmware images.
